// Child bootstrap and hook dispatcher. A separate watchdog receives control frames
// while application hooks run serially, so a blocked hook cannot disable parent-loss detection.

#include <module/base_module.hpp>
#include <vinc/control/channel.hpp>
#include "module_protocol.hpp"
#include "runtime.hpp"
#include <atomic>
#include <bit>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace vinc {
class detail::ModuleBusAccess {
public:
    static void attach(BaseModule& module, std::shared_ptr<bus::Client> client) { module.bus_ = std::move(client); }
};
namespace {
struct CompiledModule { std::string name; detail::ModuleFactory factory; bool duplicate = false; };
CompiledModule& compiled() { static CompiledModule instance; return instance; }
std::optional<std::uint64_t> number(const std::string& s) {
    std::uint64_t n = 0; auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size()) return {};
    return n;
}
}
bool detail::register_compiled_module(std::string name, ModuleFactory factory) {
    auto& entry = compiled();
    if (entry.factory) { entry.duplicate = true; return false; }
    entry.name = std::move(name); entry.factory = std::move(factory); return true;
}
// The first ten arguments are the framework bootstrap contract. Application startup
// arguments follow them unchanged; ordinary users should launch through ModuleManager.
int run_module_process(const std::vector<std::string>& args) {
    if (args.size() < 10 || args[0] != "--vinc-module-v1" || !compiled().factory || compiled().duplicate) return 64;
    const auto generation = number(args[4]), interval = number(args[5]), timeout = number(args[6]),
        startup = number(args[7]), grace = number(args[8]);
    if (!generation || !interval || !timeout || !startup || !grace || args[9] != compiled().name ||
        !*generation || !*interval || *interval >= *timeout || *timeout > 86400000 || !*startup || *startup > 86400000 ||
        !*grace || *grace > 60000 || args[2].empty()) return 64;
    using namespace std::chrono_literals;
    auto service_result = control::Service::create(1);
    if (!service_result) return 65;
    auto service = std::move(*service_result);
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket) return 65;
    if (!socket->connect_until(osal::LocalEndpoint{args[1]}, control::Clock::now() + std::chrono::milliseconds(*startup))) return 65;
    control::Options options;
    options.role = control::Role::Module; options.token = args[3]; options.generation = *generation;
    options.heartbeat_interval = std::chrono::milliseconds(*interval);
    options.heartbeat_timeout = std::chrono::milliseconds(*timeout);
    options.handshake_timeout = std::chrono::milliseconds(*startup);
    auto attached = service->attach(std::move(*socket), options);
    if (!attached) return 65;
    auto session = *attached;
    if (!session->wait_open(control::Clock::now() + options.handshake_timeout)) return 66;
    auto bus_client = bus::detail::RuntimeAccess::create(session);
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<control::Frame> requests;
    bool stopping = false;
    std::atomic<bool> finished{false};
    // Independent of application constructors, hooks and destructors.
    // This watches the framework connection, not arbitrary application-thread health.
    // Loss of the session or an expired stop grace period uses _Exit because hooks may be stuck.
    std::jthread watchdog([&](std::stop_token token) {
        try {
            auto stop_deadline = (control::Deadline::max)();
            while (!token.stop_requested()) {
                auto frame = session->receive_until(stop_deadline);
                if (finished.load()) return;
                if (control::Clock::now() >= stop_deadline) std::_Exit(73);
                if (!frame) {
                    if (frame.error().code == control::ErrorCode::Timeout && session->state() != control::State::Closed) continue;
                    std::_Exit(72);
                }
                if (frame->domain == bus::detail::bus_domain) {
                    if (!bus::detail::RuntimeAccess::receive(*bus_client, std::move(*frame))) session->close();
                } else if (frame->kind == control::Kind::Event && frame->domain == detail::lifecycle && frame->operation == detail::stop) {
                    if (!frame->payload.empty()) std::_Exit(74);
                    std::lock_guard lock(mutex);
                    if (!stopping) { stopping = true; stop_deadline = control::Clock::now() + std::chrono::milliseconds(*grace); }
                    changed.notify_all();
                } else if (frame->kind == control::Kind::Request) {
                    std::lock_guard lock(mutex);
                    if (requests.size() >= 32) { session->close(); continue; }
                    requests.push_back(std::move(*frame)); changed.notify_all();
                } else { session->close(); }
            }
        } catch (...) { std::_Exit(75); }
    });
    int result = 0;
    try {
        auto node = compiled().factory(args[2]);
        if (!node || node->get_node_name() != args[2]) throw std::runtime_error("invalid module factory");
        detail::ModuleBusAccess::attach(*node, bus_client);
        // Attach framework services before user startup. Ready is sent only after on_start
        // returns success, so central callers can distinguish a live process from a usable module.
        result = node->on_start(std::vector<std::string>(args.begin() + 10, args.end()));
        if (!result) {
            control::Frame ready{control::Kind::Event, detail::lifecycle, detail::ready};
            control::put_string(ready.payload, node->get_node_name());
            if (!session->send(std::move(ready))) throw std::runtime_error("send ready failed");
            while (true) {
                control::Frame request;
                { std::unique_lock lock(mutex);
                  changed.wait(lock, [&] { return stopping || !requests.empty(); });
                  if (stopping) break;
                  request = std::move(requests.front()); requests.pop_front(); }
                // Keep each response tied to the original domain, operation and request ID.
                // Hook exceptions become status codes instead of escaping the dispatch loop.
                control::Frame response{control::Kind::Response, request.domain, request.operation, request.request};
                int32_t status = detail::unsupported_operation;
                std::vector<std::string> completions;
                try {
                    if (request.domain == detail::commands) {
                        auto decoded = control::decode_arguments(request.payload);
                        if (!decoded) { session->close(); continue; }
                        if (request.operation == detail::execute) status = node->node_interface(std::move(*decoded));
                        else if (request.operation == detail::complete) { completions = node->node_autoComplete(std::move(*decoded)); status = 0; }
                    }
                } catch (...) { status = detail::hook_exception; }
                control::put_u32(response.payload, std::bit_cast<std::uint32_t>(status));
                if (request.domain == detail::commands && request.operation == detail::complete && !status) {
                    try { auto payload = control::encode_arguments(completions); response.payload.insert(response.payload.end(), payload.begin(), payload.end()); }
                    catch (...) { response.payload.clear(); control::put_u32(response.payload, std::bit_cast<std::uint32_t>(detail::hook_exception)); }
                }
                if (!session->send(std::move(response))) session->close();
            }
        }
        node->on_stop(); node.reset();
    } catch (...) { result = 70; }
    finished.store(true); bus::detail::RuntimeAccess::shutdown(*bus_client); session->close(); watchdog.request_stop(); watchdog.join();
    return result;
}
}
