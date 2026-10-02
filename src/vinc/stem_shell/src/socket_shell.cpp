#include <stem_shell/socket_shell.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/output/text_renderer.hpp>
#include <array>
#include <algorithm>
#include <deque>
#include <thread>

namespace vinc::shell {
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t queue_limit = 1024 * 1024;
constexpr std::size_t payload_limit = 256 * 1024;
constexpr std::size_t reply_reserve = 600 * 1024;
template<class T> T require(osal::Result<T> result) {
    if (!result) throw std::runtime_error("Socket Shell OS error " + std::to_string(static_cast<int>(result.error().code)) +
        ": " + result.error().native.message());
    return std::move(*result);
}
void require(osal::Result<void> result) {
    if (!result) throw std::runtime_error("Socket Shell OS error " + std::to_string(static_cast<int>(result.error().code)) +
        ": " + result.error().native.message());
}
std::string token() {
    std::array<std::byte, 32> bytes{}; require(osal::random_bytes(bytes));
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { auto n = std::to_integer<unsigned>(byte); result += hex[n >> 4]; result += hex[n & 15]; }
    return result;
}
// Reject malformed UTF-8 and terminal control characters before command dispatch.
bool valid_line(std::string_view line) {
    for (std::size_t i = 0; i < line.size();) {
        auto c = static_cast<unsigned char>(line[i++]);
        if (c < 128) { if ((c < 32 && c != '\t') || c == 127) return false; continue; }
        unsigned n = 0, cp = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; min = 128; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; min = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; min = 65536; }
        else return false;
        if (line.size() - i < n) return false;
        while (n--) { c = static_cast<unsigned char>(line[i++]); if ((c & 0xc0) != 0x80) return false; cp = (cp << 6) | (c & 63); }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 128 && cp < 160)) return false;
    }
    return true;
}
std::string wire_text(std::string_view input) {
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == '\r') { if (i + 1 < input.size() && input[i + 1] == '\n') ++i; out += "\r\n"; }
        else if (c == '\n') out += "\r\n";
        else if ((c < 32 && c != '\t') || c == 127) {
            constexpr char hex[] = "0123456789ABCDEF"; out += "\\x"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out;
}
}
struct SocketShell::Impl {
    struct Session {
        std::uint64_t id;
        std::string code;
        osal::Socket socket;
        osal::SocketRegistration registration{};
        osal::Deadline detached{}, progress = Clock::now();
        bool expired = false, provisional = false, drop = false, cr = false, oversized = false, output_on = true;
        bool output_drained = false, final_announced = false;
        std::string line;
        std::deque<std::string> replies;
        std::size_t queued = 0;
        std::string sending;
        std::size_t offset = 0;
        output::Subscription output;
        output::TextRenderer renderer;
        std::future<CommandResult> job;
        std::stop_source stop;
        explicit Session(std::uint64_t value) : id(value), code(token()) {}
    };
    ModuleManager& manager;
    ScriptService& scripts;
    CommandRouter router;
    SocketShellOptions options;
    osal::Socket listener;
    osal::SocketPoller poller;
    osal::SocketRegistration listening{};
    osal::IpEndpoint bound;
    std::vector<std::unique_ptr<Session>> sessions;
    std::uint64_t next_id = 2;
    bool draining = false; // Network-thread state: all command workers have returned.
    std::atomic<bool> requested{false}, stopping{false}, finishing{false}, aborting{false};
    mutable std::mutex failure_mutex;
    std::string failure_message;
    std::jthread worker;

    Impl(ModuleManager& m, ScriptService& s, SocketShellOptions o)
        : manager(m), scripts(s), router(m), options(std::move(o)), poller(require(osal::SocketPoller::create(9))) {
        if (options.max_connections < 1 || options.max_connections > 8 || options.disconnect_timeout < 0ms || options.send_timeout <= 0ms)
            throw std::invalid_argument("Invalid Socket Shell limits (1..8 connections required)");
        osal::IpEndpoint address; address.port = options.port;
        if (auto ip = osal::IPv4Address::parse(options.ip)) address.address = *ip;
        else if (auto ip6 = osal::IPv6Address::parse(options.ip)) address.address = *ip6;
        else throw std::invalid_argument("--shell-ip requires a numeric IPv4 or IPv6 address");
        listener = require(osal::Socket::open(osal::address_family(address), osal::SocketType::Stream));
#ifdef _WIN32
        require(listener.set_option<osal::socket_options::ExclusiveAddressUse>(true));
#else
        require(listener.set_option<osal::socket_options::ReuseAddress>(true));
#endif
        require(listener.bind(address)); require(listener.listen(static_cast<int>(options.max_connections)));
        bound = std::get<osal::IpEndpoint>(require(listener.local_address()));
        listening = require(poller.add(listener, osal::SocketEvents::Readable));
        sessions.reserve(options.max_connections * 2 + 1);
        worker = std::jthread([this] { run(); });
    }
    ~Impl() { stopping.store(true); aborting.store(true); if (worker.joinable()) worker.join(); }
    // Account for the complete in-flight item as well as queued items. Partial writes
    // never allow another reply or a module output chunk to split that item.
    bool enqueue(Session& s, std::string text) {
        if (text.size() > payload_limit) text = "Error: reply exceeds the 256 KiB Shell limit";
        text = wire_text(text);
        if (text.size() + s.queued + s.sending.size() > queue_limit || s.replies.size() >= 128) { s.drop = true; return false; }
        if (s.replies.empty() && s.sending.empty()) s.progress = Clock::now();
        s.queued += text.size(); s.replies.push_back(std::move(text)); return true;
    }
    void reply(Session& s, std::string text) { enqueue(s, std::move(text) + "\nVINC > "); }
    void subscribe(Session& s) {
        if (!s.output_on) return;
        auto sub = manager.output_hub().subscribe({128 * 1024, 64});
        if (sub) s.output = std::move(*sub);
        else { s.output_on = false; reply(s, "Output subscription unavailable; use output on to retry"); }
    }
    void detach(Session& s) {
        if (!s.socket) return;
        if (s.registration.value) (void)poller.remove(s.registration);
        s.registration = {};
        (void)s.socket.close(); s.output.close();
        s.detached = Clock::now(); s.line.clear(); s.replies.clear(); s.sending.clear(); s.queued = s.offset = 0;
        s.drop = false; s.cr = s.oversized = false;
        if (s.provisional) s.expired = true;
    }
    void accept() {
        for (std::size_t quota = 0; quota < 8; ++quota) {
            auto incoming = listener.try_accept();
            if (!incoming) return;
            const auto count = std::count_if(sessions.begin(), sessions.end(), [](const auto& s) { return bool(s->socket); });
            if (count >= static_cast<std::ptrdiff_t>(options.max_connections) || sessions.size() >= options.max_connections * 2 + 1) continue;
            auto s = std::make_unique<Session>(next_id++);
            s->provisional = sessions.size() >= options.max_connections * 2;
            s->socket = std::move(*incoming);
            (void)s->socket.set_option<osal::socket_options::NoDelay>(true);
            s->registration = require(poller.add(s->socket, osal::SocketEvents::Readable, s->id));
            if (!s->provisional) subscribe(*s);
            reply(*s, "VINC Socket Shell (Raw TCP). disconnect leaves this connection; exit shuts down Vinc.\nResume code: " + s->code +
                (s->provisional ? "\nSession storage full: only resume is available." : ""));
            sessions.push_back(std::move(s));
        }
    }
    // Restore ownership, not just an IP address. Rotate the bearer code after use.
    bool resume(Session& fresh, const std::vector<std::string>& args) {
        if (args.size() != 2) { reply(fresh, "Usage: resume <code>"); return false; }
        if (fresh.job.valid() || scripts.active(fresh.id)) { reply(fresh, "Busy: this session already owns work"); return false; }
        for (auto& old : sessions) {
            if (old.get() == &fresh || old->socket || old->expired || old->code != args[1] || Clock::now() - old->detached >= options.disconnect_timeout) continue;
            auto replacement = token();
            (void)poller.remove(fresh.registration); fresh.output.close();
            old->socket = std::move(fresh.socket);
            old->registration = require(poller.add(old->socket, osal::SocketEvents::Readable, old->id));
            old->code = std::move(replacement); old->renderer = {}; old->progress = Clock::now();
            subscribe(*old); fresh.expired = true;
            reply(*old, "Session resumed. Disconnected output was not retained.\nResume code: " + old->code);
            return true;
        }
        reply(fresh, "Resume rejected: code unavailable or expired"); return false;
    }
    void command(Session& s, std::string line) {
        if (!valid_line(line)) { reply(s, "Rejected invalid UTF-8 or control character"); return; }
        auto parsed = parse_line(line);
        if (!parsed) { reply(s, "Parse error: " + parsed.error()); return; }
        auto& args = parsed->arguments;
        if (args.empty()) { reply(s, ""); return; }
        const auto& name = args.front();
        if (name == "resume") { resume(s, args); return; }
        if (s.provisional) { reply(s, "Session storage full: only resume is available"); return; }
        if (name == "disconnect") {
            if (args.size() != 1) reply(s, "disconnect takes no arguments"); else s.drop = true;
            return;
        }
        if (name == "scancel") {
            if (args.size() != 1) reply(s, "scancel takes no arguments");
            else reply(s, scripts.cancel() ? "Script cancellation requested" : "No script is running");
            return;
        }
        if (name == "sstatus") {
            reply(s, args.size() != 1 ? "sstatus takes no arguments" : scripts.active() ? "Script running" : "No script is running"); return;
        }
        if (name == "output") {
            if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) { reply(s, "Usage: output on|off"); return; }
            s.output.close(); s.output_on = args[1] == "on"; s.renderer = {};
            subscribe(s); reply(s, s.output_on ? "Module output on" : "Module output off"); return;
        }
        if (stopping.load()) { reply(s, "Framework is shutting down"); return; }
        if (name == "exit" && args.size() == 1) { reply(s, "Shutting down: waiting for all module processes..."); requested.store(true); return; }
        if (s.job.valid()) { reply(s, "Busy: this session already has a command in flight"); return; }
        if (name == "srun") {
            if (args.size() < 2) { reply(s, "Usage: srun file.tcl [args...]"); return; }
            const auto& path = args[1];
            auto result = scripts.start(s.id, std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                {args.begin() + 2, args.end()});
            reply(s, result.message); return;
        }
        s.stop = {};
        s.job = std::async(std::launch::async, [this, args = std::move(args), stop = s.stop.get_token()] {
            try { return router.execute(args, stop); }
            catch (const std::exception& e) { return CommandResult{false, false, std::string("Command error: ") + e.what()}; }
            catch (...) { return CommandResult{false, false, "Unknown command exception"}; }
        });
    }
    void receive(Session& s) {
        std::array<std::byte, 4096> bytes{};
        auto got = s.socket.try_receive(bytes);
        if (!got) { if (got.error().code != osal::ErrorCode::WouldBlock) s.drop = true; return; }
        if (got->eof) { s.drop = true; return; }
        for (std::size_t i = 0; i < got->bytes && s.socket && !s.drop; ++i) {
            const auto c = static_cast<char>(bytes[i]);
            if (c == '\n' && s.cr) { s.cr = false; continue; }
            s.cr = c == '\r';
            if (c == '\r' || c == '\n') {
                if (s.oversized) reply(s, "Rejected oversized command (maximum 4096 bytes)");
                else command(s, std::move(s.line));
                s.line.clear(); s.oversized = false;
            } else if (!s.oversized) {
                if (s.line.size() == max_line_bytes) { s.line.clear(); s.oversized = true; }
                else s.line += c;
            }
        }
    }
    void transmit(Session& s) {
        for (int quota = 0; quota < 8; ++quota) {
            if (s.sending.empty()) {
                if (s.replies.empty()) return;
                s.sending = std::move(s.replies.front()); s.replies.pop_front(); s.queued -= s.sending.size(); s.offset = 0;
            }
            auto sent = s.socket.try_send(std::as_bytes(std::span(s.sending)).subspan(s.offset));
            if (!sent) { if (sent.error().code != osal::ErrorCode::WouldBlock) s.drop = true; return; }
            if (!*sent) return;
            s.offset += *sent; s.progress = Clock::now();
            if (s.offset == s.sending.size()) { s.sending.clear(); s.offset = 0; }
        }
    }
    void collect(Session& s) {
        if (stopping.load()) s.stop.request_stop();
        const bool room = s.queued + s.sending.size() < queue_limit - reply_reserve && s.replies.size() < 120;
        if ((room || !s.socket) && s.job.valid() && s.job.wait_for(0ms) == std::future_status::ready) {
            auto result = s.job.get(); if (result.shutdown) requested.store(true);
            if (s.socket) reply(s, result.message.empty() ? (result.success ? "OK" : "Command failed") : result.message);
        }
        if (room || !s.socket) {
            auto delivery = scripts.take(s.id);
            if (s.socket) {
                if (!delivery.output.empty()) enqueue(s, std::move(delivery.output));
                if (delivery.truncated) enqueue(s, "\n[Script output truncated]\n");
                if (delivery.result) reply(s, delivery.result->message);
            }
        }
        // Module output is already lossy in OutputHub. Consume only when no reliable
        // reply is waiting, preserving responsiveness and bounding network storage.
        if (s.socket && s.output_on && !s.output_drained && s.sending.empty() && s.replies.empty()) {
            auto batch = s.output.take(4);
            auto text = s.renderer.render(batch);
            if (draining && batch.events.size() < 4) {
                text += s.renderer.flush(Clock::now(), true); s.output_drained = true;
            }
            if (!text.empty()) enqueue(s, std::move(text));
        }
        if (draining && s.socket && !s.final_announced && (!s.output_on || s.output_drained)) {
            reply(s, "All module processes exited; framework shutdown complete."); s.final_announced = true;
        }
    }
    void run() noexcept {
        try { loop(); }
        catch (const std::exception& e) {
            { std::lock_guard lock(failure_mutex); failure_message = e.what(); }
            requested.store(true);
        } catch (...) {
            { std::lock_guard lock(failure_mutex); failure_message = "Unknown Socket Shell worker failure"; }
            requested.store(true);
        }
        if (listener) { (void)poller.remove(listening); (void)listener.close(); }
        for (auto& s : sessions) { detach(*s); s->stop.request_stop(); scripts.cancel(s->id); scripts.forget(s->id); }
        // Future destruction joins outstanding native command callbacks. Never detach
        // a callback that still references the router or manager.
        sessions.clear();
    }
    void loop() {
        std::array<osal::SocketReadyEvent, 16> ready{};
        std::optional<osal::Deadline> final_deadline;
        for (;;) {
            if (aborting.load()) break;
            if (stopping.load() && listener) { require(poller.remove(listening)); require(listener.close()); }
            if (finishing.load() && !final_deadline && std::none_of(sessions.begin(), sessions.end(), [](const auto& s) { return s->job.valid(); })) {
                draining = true;
                final_deadline = Clock::now() + 2s;
            }
            bool pending = false;
            for (auto& ptr : sessions) {
                auto& s = *ptr; collect(s);
                if (s.socket) {
                    if ((!s.sending.empty() || !s.replies.empty()) && Clock::now() - s.progress >= options.send_timeout) s.drop = true;
                    if (s.drop) detach(s);
                    else {
                        auto interests = osal::SocketEvents::None;
                        if (!stopping.load() && s.queued + s.sending.size() < queue_limit - reply_reserve) interests = osal::SocketEvents::Readable;
                        if (!s.sending.empty() || !s.replies.empty()) interests = interests | osal::SocketEvents::Writable;
                        // OSAL deliberately rejects an empty watch. Unregister idle
                        // shutdown connections and add a new watch if a final reply arrives.
                        if (interests == osal::SocketEvents::None) {
                            if (s.registration.value) require(poller.remove(s.registration));
                            s.registration = {};
                        } else if (!s.registration.value) s.registration = require(poller.add(s.socket, interests, s.id));
                        else require(poller.modify(s.registration, interests));
                        pending |= !s.sending.empty() || !s.replies.empty();
                    }
                }
                if (!s.socket && !s.expired && Clock::now() - s.detached >= options.disconnect_timeout) {
                    s.expired = true; scripts.cancel(s.id); scripts.forget(s.id); s.stop.request_stop();
                }
                pending |= s.job.valid();
                pending |= finishing.load() && s.socket && !s.final_announced;
            }
            std::erase_if(sessions, [&](const auto& s) {
                if (!s->expired || s->job.valid()) return false;
                scripts.forget(s->id); return true;
            });
            if (final_deadline && (!pending || Clock::now() >= *final_deadline)) break;
            auto events = poller.wait_until(ready, Clock::now() + 20ms);
            if (!events) { if (events.error().code == osal::ErrorCode::Timeout) continue; require(std::move(events)); }
            for (std::size_t i = 0; i < *events; ++i) {
                const auto& event = ready[i];
                if (listener && event.registration == listening) { if (!stopping.load()) accept(); continue; }
                const auto it = std::find_if(sessions.begin(), sessions.end(), [&](const auto& s) { return s->socket && s->registration == event.registration; });
                if (it == sessions.end()) continue;
                auto& s = **it;
                if (osal::has_event(event.events, osal::SocketEvents::Readable)) receive(s);
                if (s.socket && !s.drop && osal::has_event(event.events, osal::SocketEvents::Writable)) transmit(s);
                if (osal::has_event(event.events, osal::SocketEvents::Error) || osal::has_event(event.events, osal::SocketEvents::Hangup)) s.drop = true;
            }
        }
    }
};
SocketShell::SocketShell(ModuleManager& m, ScriptService& s, SocketShellOptions o) : impl_(std::make_unique<Impl>(m, s, std::move(o))) {}
SocketShell::~SocketShell() = default;
osal::IpEndpoint SocketShell::endpoint() const { return impl_->bound; }
bool SocketShell::shutdown_requested() const noexcept { return impl_->requested.load(); }
std::string SocketShell::failure() const { std::lock_guard lock(impl_->failure_mutex); return impl_->failure_message; }
void SocketShell::begin_shutdown() { impl_->stopping.store(true); }
void SocketShell::finish_shutdown() { impl_->stopping.store(true); impl_->finishing.store(true); if (impl_->worker.joinable()) impl_->worker.join(); }
}
