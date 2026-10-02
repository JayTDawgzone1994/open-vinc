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
    static void attach(BaseModule& module, std::shared_ptr<bus::Client> client) {
        module.bus_ = std::move(client);
    }

    static void identify(BaseModule& module, MID mid) {
        module.mid_ = mid;
    }

    static void name(BaseModule& module, std::string name) {
        module.module_name_ = std::move(name);
    }
};

namespace {
struct CompiledModuleType {
    std::string name;
    detail::ModuleFactory factory;
    bool duplicate = false;
};

CompiledModuleType& compiled() {
    static CompiledModuleType instance;
    return instance;
}

std::optional<std::uint64_t> parse_number(const std::string& text) {
    std::uint64_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return {};
    }

    return value;
}

// Per-process limits; values and child exit statuses preserve the existing bootstrap ABI.
constexpr std::size_t pending_request_limit = 32;
constexpr std::size_t central_session_limit = 1;
constexpr std::uint64_t maximum_bootstrap_timeout_ms = 86400000;
constexpr std::uint64_t maximum_stop_grace_ms = 60000;

namespace child_exit {
constexpr int invalid_bootstrap = 64;
constexpr int connection_failed = 65;
constexpr int handshake_failed = 66;
constexpr int application_exception = 70;
constexpr int central_lost = 72;
constexpr int stop_deadline_expired = 73;
constexpr int invalid_stop_message = 74;
constexpr int watchdog_exception = 75;
}

struct Bootstrap {
    std::uint64_t generation;
    std::uint64_t interval;
    std::uint64_t timeout;
    std::uint64_t startup;
    std::uint64_t grace;
};

// Shared only by the watchdog and main dispatcher. The mutex protects every
// non-atomic field; finished keeps its existing sequentially consistent ordering.
struct RuntimeInbox {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<control::Frame> requests;
    bool stopping = false;
    std::optional<control::Frame> registration;
    std::atomic<bool> finished{false};
};

std::optional<Bootstrap> parse_bootstrap(const std::vector<std::string>& args) {
    if (args.size() < detail::bootstrap::argument_count ||
        args[detail::bootstrap::marker] != detail::bootstrap::version_marker || !compiled().factory ||
        compiled().duplicate) {
        return {};
    }

    const auto generation = parse_number(args[detail::bootstrap::generation]);
    const auto interval = parse_number(args[detail::bootstrap::heartbeat_interval]);
    const auto timeout = parse_number(args[detail::bootstrap::heartbeat_timeout]);
    const auto startup = parse_number(args[detail::bootstrap::startup_timeout]);
    const auto grace = parse_number(args[detail::bootstrap::stop_timeout]);
    if (!generation || !interval || !timeout || !startup || !grace) {
        return {};
    }

    if (args[detail::bootstrap::module_type] != compiled().name) {
        return {};
    }

    const bool invalid_heartbeat =
        !*generation || !*interval || *interval >= *timeout || *timeout > maximum_bootstrap_timeout_ms;
    if (invalid_heartbeat) {
        return {};
    }

    const bool invalid_startup_timeout = !*startup || *startup > maximum_bootstrap_timeout_ms;
    if (invalid_startup_timeout) {
        return {};
    }

    const bool invalid_stop_timeout = !*grace || *grace > maximum_stop_grace_ms;
    if (invalid_stop_timeout || args[detail::bootstrap::base_name].empty()) {
        return {};
    }

    return Bootstrap{*generation, *interval, *timeout, *startup, *grace};
}

// Socket ownership transfers to Service only after a successful attach. The caller
// keeps Service alive through watchdog shutdown; failed setup releases local resources.
std::shared_ptr<control::Session> connect_to_central(control::Service& service,
                                                     const std::vector<std::string>& args,
                                                     const Bootstrap& bootstrap, control::Options& options) {
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket) {
        return {};
    }

    if (!socket->connect_until(osal::LocalEndpoint{args[detail::bootstrap::endpoint]},
                               control::Clock::now() + std::chrono::milliseconds(bootstrap.startup))) {
        return {};
    }

    options.role = control::Role::Module;
    options.token = args[detail::bootstrap::token];
    options.generation = bootstrap.generation;
    options.heartbeat_interval = std::chrono::milliseconds(bootstrap.interval);
    options.heartbeat_timeout = std::chrono::milliseconds(bootstrap.timeout);
    options.handshake_timeout = std::chrono::milliseconds(bootstrap.startup);
    auto attached = service.attach(std::move(*socket), options);
    if (!attached) {
        return {};
    }

    return *attached;
}

// Runs independently of constructors, hooks and destructors. Session loss and an
// expired stop grace use _Exit because application code may be stuck. No user hook runs here.
void watch_central(const std::shared_ptr<control::Session>& session, bus::Client& bus_client,
                   RuntimeInbox& inbox, std::chrono::milliseconds stop_grace, std::stop_token token) {
    try {
        auto stop_deadline = (control::Deadline::max)();
        while (!token.stop_requested()) {
            auto frame = session->receive_until(stop_deadline);
            if (inbox.finished.load()) {
                return;
            }

            if (control::Clock::now() >= stop_deadline) {
                std::_Exit(child_exit::stop_deadline_expired);
            }
            if (!frame) {
                if (frame.error().code == control::ErrorCode::Timeout &&
                    session->state() != control::State::Closed) {
                    continue;
                }

                std::_Exit(child_exit::central_lost);
            }
            if (frame->kind == control::Kind::Response && frame->domain == detail::lifecycle &&
                frame->operation == detail::register_name &&
                frame->request == detail::name_registration_request) {
                std::lock_guard lock(inbox.mutex);
                if (inbox.registration) {
                    session->close();
                    continue;
                }

                inbox.registration = std::move(*frame);
                inbox.changed.notify_all();
            } else if (frame->domain == bus::detail::bus_domain) {
                if (!bus::detail::RuntimeAccess::receive(bus_client, std::move(*frame))) {
                    session->close();
                }
            } else if (frame->kind == control::Kind::Event && frame->domain == detail::lifecycle &&
                       frame->operation == detail::stop) {
                if (!frame->payload.empty()) {
                    std::_Exit(child_exit::invalid_stop_message);
                }
                std::lock_guard lock(inbox.mutex);
                if (!inbox.stopping) {
                    inbox.stopping = true;
                    stop_deadline = control::Clock::now() + stop_grace;
                }
                inbox.changed.notify_all();
            } else if (frame->kind == control::Kind::Request) {
                std::lock_guard lock(inbox.mutex);
                if (inbox.requests.size() >= pending_request_limit) {
                    session->close();
                    continue;
                }

                inbox.requests.push_back(std::move(*frame));
                inbox.changed.notify_all();
            } else {
                session->close();
            }
        }
    } catch (...) {
        // Preserve the existing fatal boundary; detailed diagnostics are tracked in README.
        std::_Exit(child_exit::watchdog_exception);
    }
}

// The watchdog receives the acknowledgement while this thread waits. Keep name
// assignment inside the inbox lock, then attach Bus only after central acceptance.
void register_module_name(BaseModule& module, std::vector<std::string>& startup_args,
                          const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox,
                          std::chrono::milliseconds registration_timeout) {
    const auto proposed = module.on_name(startup_args);
    control::Frame claim{control::Kind::Request, detail::lifecycle, detail::register_name,
                         detail::name_registration_request};
    control::put_string(claim.payload, proposed);
    if (!session->send(std::move(claim))) {
        throw std::runtime_error("send name registration failed");
    }

    {
        std::unique_lock lock(inbox.mutex);
        const bool registration_received =
            inbox.changed.wait_until(lock, control::Clock::now() + registration_timeout, [&] {
                return inbox.registration.has_value() || inbox.stopping;
            });
        if (!registration_received || inbox.stopping) {
            throw std::runtime_error("name registration interrupted");
        }

        control::Reader reader(inbox.registration->payload);
        const auto code = reader.u32();
        const auto name = reader.string();
        if (!reader.done() || code || name != proposed) {
            throw std::runtime_error("name registration rejected");
        }

        detail::ModuleBusAccess::name(module, name);
    }
}

void dispatch_request(BaseModule& module, const control::Frame& request,
                      const std::shared_ptr<control::Session>& session) {
    // Keep each response tied to the original domain, operation and request ID.
    // Hook exceptions become status codes instead of escaping the dispatch loop.
    control::Frame response{control::Kind::Response, request.domain, request.operation, request.request};
    int32_t status = detail::unsupported_operation;
    std::vector<std::string> completions;
    try {
        if (request.domain == detail::commands) {
            auto decoded = control::decode_arguments(request.payload);
            if (!decoded) {
                session->close();
                return;
            }

            if (request.operation == detail::execute) {
                status = module.module_interface(std::move(*decoded));
            } else if (request.operation == detail::complete) {
                completions = module.module_autoComplete(std::move(*decoded));
                status = 0;
            }
        }
    } catch (...) {
        status = detail::hook_exception;
    }
    control::put_u32(response.payload, std::bit_cast<std::uint32_t>(status));
    if (request.domain == detail::commands && request.operation == detail::complete && !status) {
        try {
            auto payload = control::encode_arguments(completions);
            response.payload.insert(response.payload.end(), payload.begin(), payload.end());
        } catch (...) {
            response.payload.clear();
            control::put_u32(response.payload, std::bit_cast<std::uint32_t>(detail::hook_exception));
        }
    }
    if (!session->send(std::move(response))) {
        session->close();
    }
}

// User hooks run serially on the main thread, outside the inbox mutex. Stop wins
// over queued requests, exactly as it does during the name-registration wait.
void dispatch_commands(BaseModule& module, const std::shared_ptr<control::Session>& session,
                       RuntimeInbox& inbox) {
    while (true) {
        control::Frame request;
        {
            std::unique_lock lock(inbox.mutex);
            inbox.changed.wait(lock, [&] {
                return inbox.stopping || !inbox.requests.empty();
            });
            if (inbox.stopping) {
                break;
            }

            request = std::move(inbox.requests.front());
            inbox.requests.pop_front();
        }
        dispatch_request(module, request, session);
    }
}
} // namespace

bool detail::register_compiled_module_type(std::string name, ModuleFactory factory) {
    auto& entry = compiled();
    if (entry.factory) {
        entry.duplicate = true;
        return false;
    }

    entry.name = std::move(name);
    entry.factory = std::move(factory);
    return true;
}

// Framework bootstrap arguments precede application arguments; use ModuleManager to launch.
int run_module_process(const std::vector<std::string>& args) {
    const auto bootstrap = parse_bootstrap(args);
    if (!bootstrap) {
        return child_exit::invalid_bootstrap;
    }

    auto service_result = control::Service::create(central_session_limit);
    if (!service_result) {
        return child_exit::connection_failed;
    }

    auto service = std::move(*service_result);
    control::Options options;
    auto session = connect_to_central(*service, args, *bootstrap, options);
    if (!session) {
        return child_exit::connection_failed;
    }

    if (!session->wait_open(control::Clock::now() + options.handshake_timeout)) {
        return child_exit::handshake_failed;
    }

    auto bus_client = bus::detail::RuntimeAccess::create(session);
    RuntimeInbox inbox;
    // These referenced objects outlive the watchdog, which is explicitly joined below.
    std::jthread watchdog([&](std::stop_token token) {
        watch_central(session, *bus_client, inbox, std::chrono::milliseconds(bootstrap->grace), token);
    });

    int result = 0;
    try {
        auto module = compiled().factory(args[detail::bootstrap::base_name]);
        if (!module || module->get_module_name() != args[detail::bootstrap::base_name]) {
            throw std::runtime_error("invalid module factory");
        }

        detail::ModuleBusAccess::identify(*module, bootstrap->generation);
        auto startup_args =
            std::vector<std::string>(args.begin() + detail::bootstrap::argument_count, args.end());
        register_module_name(*module, startup_args, session, inbox, options.handshake_timeout);
        detail::ModuleBusAccess::attach(*module, bus_client);
        // Attach framework services before user startup. Ready is sent only after on_start
        // returns success, so central callers can distinguish a live process from a usable module.
        result = module->on_start(startup_args);
        if (!result) {
            control::Frame ready{control::Kind::Event, detail::lifecycle, detail::ready};
            control::put_string(ready.payload, module->get_module_name());
            if (!session->send(std::move(ready))) {
                throw std::runtime_error("send ready failed");
            }

            dispatch_commands(*module, session, inbox);
        }
        module->on_stop();
        module.reset();
    } catch (...) {
        // Startup/lifecycle exceptions retain the legacy generic exit status in this pass.
        result = child_exit::application_exception;
    }
    // Wake receive_until by closing the session before joining the watchdog.
    inbox.finished.store(true);
    bus::detail::RuntimeAccess::shutdown(*bus_client);
    session->close();
    watchdog.request_stop();
    watchdog.join();
    return result;
}
} // namespace vinc
