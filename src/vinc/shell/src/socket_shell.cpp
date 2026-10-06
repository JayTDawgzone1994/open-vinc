#include <vinc/osal/path.hpp>
#include <vinc/time.hpp>
#include <vinc/shell/detail/socket_shell.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/output_hub/text_renderer.hpp>
#include <array>
#include <algorithm>
#include <deque>
#include <thread>
#include <limits>

#ifdef VINC_SHELL_TESTING
#include "socket_hooks.hpp"
#endif

namespace vinc::shell {
using namespace std::chrono_literals;

namespace {
using Clock = std::chrono::steady_clock;
// The bearer token is 256 random bits encoded as 64 hexadecimal characters.
constexpr std::size_t resume_token_bytes = 32;
// Session 1 belongs to the local Shell; zero is ScriptService's global selector.
constexpr std::uint64_t first_remote_session_id = 2;

Result<std::string> make_resume_code() {
    std::array<std::byte, resume_token_bytes> bytes{};
    auto generated = osal::random_bytes(bytes);
    if (!generated) {
        return std::unexpected(
            wrap_error(ErrorCode::SocketFailed, ErrorOperation::GenerateToken, generated.error()));
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) {
        auto value = std::to_integer<unsigned>(byte);
        result += hex[value >> 4];
        result += hex[value & 15];
    }

    return result;
}

// Reject malformed UTF-8 and terminal control characters before command dispatch.
bool is_valid_command_line(std::string_view command_line) {
    for (std::size_t position = 0; position < command_line.size();) {
        auto byte = static_cast<unsigned char>(command_line[position++]);
        if (byte < 128) {
            if ((byte < 32 && byte != '\t') || byte == 127) {
                return false;
            }

            continue;
        }

        unsigned remaining_bytes = 0;
        unsigned code_point = 0;
        unsigned minimum_code_point = 0;
        if (byte >= 0xc2 && byte <= 0xdf) {
            remaining_bytes = 1;
            code_point = byte & 31;
            minimum_code_point = 128;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            remaining_bytes = 2;
            code_point = byte & 15;
            minimum_code_point = 2048;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            remaining_bytes = 3;
            code_point = byte & 7;
            minimum_code_point = 65536;
        } else {
            return false;
        }

        if (command_line.size() - position < remaining_bytes) {
            return false;
        }

        while (remaining_bytes--) {
            byte = static_cast<unsigned char>(command_line[position++]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }

            code_point = (code_point << 6) | (byte & 63);
        }

        if (code_point < minimum_code_point || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff) || (code_point >= 128 && code_point < 160)) {
            return false;
        }
    }

    return true;
}

std::string encode_wire_text(std::string_view input) {
    std::string encoded;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto byte = static_cast<unsigned char>(input[i]);
        if (byte == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                ++i;
            }

            encoded += "\r\n";
        } else if (byte == '\n') {
            encoded += "\r\n";
        } else if ((byte < 32 && byte != '\t') || byte == 127) {
            constexpr char hex[] = "0123456789ABCDEF";
            encoded += "\\x";
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        } else {
            encoded += static_cast<char>(byte);
        }
    }

    return encoded;
}
}

struct SocketShell::Impl {
    // Only the network worker mutates sessions. A disconnected session may retain
    // a command future and script ownership until its grace period expires.
    struct Session {
        std::uint64_t id;
        std::uint64_t contract_cursor = 0;
        std::string resume_code;
        osal::Socket socket;
        osal::SocketRegistration registration{};
        vinc::Deadline disconnected_at{};
        // Reset when an empty send queue gets work and whenever a send makes progress.
        vinc::Deadline last_send_progress = Clock::now();
        // Provisional sessions can only resume an owner; expired sessions may be
        // erased once their command future no longer owns work.
        bool expired = false;
        bool provisional = false;
        bool disconnect_requested = false;
        bool previous_was_cr = false;
        bool line_oversized = false;
        bool output_enabled = true;
        bool output_drained = false;
        bool final_announced = false;
        std::string command_line;
        std::deque<std::string> replies;
        // queued_bytes excludes sending_text; backpressure counts that whole item
        // until transmission completes, even after some bytes have reached TCP.
        std::size_t queued_bytes = 0;
        std::string sending_text;
        std::size_t send_offset = 0;
        output_hub::Subscription output;
        output_hub::TextRenderer renderer;
        // Destruction joins the callback before its router/manager can be destroyed.
        std::future<Result<CommandReply>> job;
        std::stop_source stop;

        Session(std::uint64_t value, std::string code) : id(value), resume_code(std::move(code)) {
        }
    };

    ModuleManager& manager;
    ScriptService& scripts;
    CommandRouter router;
    SocketShellOptions options;
    osal::Socket listener;
    osal::SocketPoller poller;
    osal::SocketRegistration listener_registration{};
    osal::IpEndpoint bound_endpoint;
    std::vector<std::unique_ptr<Session>> sessions;
    std::uint64_t next_session_id = first_remote_session_id;
    bool draining = false; // Network-thread state: all command workers have returned.
    // Cross-thread requests; all Session fields are owned by the network worker.
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> shutdown_started{false};
    std::atomic<bool> finish_requested{false};
    std::atomic<bool> abort_requested{false};
    mutable std::mutex failure_mutex;
    SocketShellStatus service_status;
    std::optional<vinc::Deadline> accept_retry_at;
    std::chrono::milliseconds accept_retry_delay = config::shell_socket_accept_retry_initial;
    std::jthread worker;

    Impl(ModuleManager& module_manager, ScriptService& script_service, SocketShellOptions socket_options,
         osal::SocketPoller socket_poller)
        : manager(module_manager), scripts(script_service), router(module_manager),
          options(std::move(socket_options)), poller(std::move(socket_poller)) {
    }

    Result<void> initialize() {
        auto valid = validate_options();
        if (!valid) {
            return valid;
        }

        auto reserved = router.initialize();
        if (!reserved) {
            return reserved;
        }

        auto address = parse_listen_address();
        if (!address) {
            return std::unexpected(address.error());
        }

        auto opened = open_listener(*address);
        if (!opened) {
            return opened;
        }

        auto registered = register_listener();
        if (!registered) {
            return registered;
        }

        reserve_sessions();
        return {};
    }

    std::size_t retained_session_limit() const {
        return options.max_connections * config::shell_socket_retained_session_factor;
    }

    Result<void> validate_options() const {
        if (options.max_connections < 1 || options.max_connections > config::shell_socket_max_connections ||
            options.disconnect_timeout < 0ms || options.send_timeout <= 0ms) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::StartSocket});
        }

        return {};
    }

    Result<osal::IpEndpoint> parse_listen_address() const {
        osal::IpEndpoint address;
        address.port = options.port;
        if (auto ip = osal::IPv4Address::parse(options.ip)) {
            address.address = *ip;
        } else if (auto ip6 = osal::IPv6Address::parse(options.ip)) {
            address.address = *ip6;
        } else {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::StartSocket});
        }

        return address;
    }

    // Members retain acquired resources until the factory commits ownership.
    // The worker starts only after every acquisition and registration succeeds.
    Result<void> open_listener(const osal::IpEndpoint& address) {
        auto opened = osal::Socket::open(osal::address_family(address), osal::SocketType::Stream);
        if (!opened) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::OpenSocket, opened.error()));
        }

        listener = std::move(*opened);
#ifdef _WIN32
        auto configured = listener.set_option<osal::socket_options::ExclusiveAddressUse>(true);
#else
        auto configured = listener.set_option<osal::socket_options::ReuseAddress>(true);
#endif
        if (!configured) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::OpenSocket, configured.error()));
        }

        auto bound = listener.bind(address);
        if (!bound) {
            return std::unexpected(wrap_error(ErrorCode::SocketFailed, ErrorOperation::Bind, bound.error()));
        }

        auto listening = listener.listen(static_cast<int>(options.max_connections));
        if (!listening) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::Listen, listening.error()));
        }

        auto endpoint = listener.local_address();
        if (!endpoint) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::GetEndpoint, endpoint.error()));
        }

        bound_endpoint = std::get<osal::IpEndpoint>(*endpoint);
        return {};
    }

    Result<void> register_listener() {
        auto added = poller.add(listener, osal::SocketEvents::Readable);
        if (!added) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, added.error()));
        }

        listener_registration = *added;
        {
            std::lock_guard lock(failure_mutex);
            service_status.accepting = true;
        }
        return {};
    }

    void reserve_sessions() {
        sessions.reserve(retained_session_limit() + config::shell_socket_resume_slots);
    }

    void start_worker() {
        {
            std::lock_guard lock(failure_mutex);
            service_status.running = true;
        }
        worker = std::jthread([this] {
            run();
        });
    }

    ~Impl() {
        shutdown_started.store(true);
        abort_requested.store(true);
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Account for the complete in-flight item as well as queued items. Partial writes
    // never allow another reply or a module output chunk to split that item.
    bool enqueue(Session& session, std::string text) {
        if (text.size() > config::shell_socket_payload_bytes) {
            text = "Error: reply exceeds the 256 KiB Shell limit";
        }

        text = encode_wire_text(text);
        if (text.size() + session.queued_bytes + session.sending_text.size() >
                config::shell_socket_queue_bytes ||
            session.replies.size() + (session.sending_text.empty() ? 0u : 1u) >=
                config::shell_socket_queue_items) {
            session.disconnect_requested = true;
            return false;
        }

        if (session.replies.empty() && session.sending_text.empty()) {
            session.last_send_progress = Clock::now();
        }

        session.queued_bytes += text.size();
        session.replies.push_back(std::move(text));
        return true;
    }

    void reply(Session& session, std::string text) {
        enqueue(session, std::move(text) + "\nVINC > ");
    }

    void subscribe(Session& session) {
        if (!session.output_enabled) {
            return;
        }

        auto subscription = manager.output_hub().subscribe(
            {config::shell_socket_output_queue_bytes, config::shell_socket_output_queue_events});
        if (subscription) {
            session.output = std::move(*subscription);
        } else {
            session.output_enabled = false;
            reply(session,
                  vinc::error_to_string(wrap_error(ErrorCode::OutputUnavailable,
                                                   ErrorOperation::SubscribeOutput, subscription.error())) +
                      "; use output on to retry");
        }
    }

    void detach(Session& session) {
        if (!session.socket) {
            return;
        }

        if (session.registration.value) {
            (void)poller.remove(session.registration);
        }

        session.registration = {};
        (void)session.socket.close();
        session.output.close();
        session.disconnected_at = Clock::now();
        session.command_line.clear();
        session.replies.clear();
        session.sending_text.clear();
        session.send_offset = 0;
        session.queued_bytes = 0;
        session.disconnect_requested = false;
        session.line_oversized = false;
        session.previous_was_cr = false;
        if (session.provisional) {
            session.expired = true;
        }
    }

    // The session owns its socket before registration. A registration failure
    // reaches the worker boundary, whose cleanup closes retained sessions.
    Result<void> register_session(Session& session) {
        auto added = poller.add(session.socket, osal::SocketEvents::Readable, session.id);
        if (!added) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, added.error()));
        }

        session.registration = *added;
        return {};
    }

    void record_failure(const Error& error) {
        std::lock_guard lock(failure_mutex);
        service_status.last_failure = error;
        if (service_status.failures != (std::numeric_limits<std::uint64_t>::max)()) {
            ++service_status.failures;
        }
    }

    Result<void> unwatch_listener() {
        if (listener_registration.value) {
            auto removed = poller.remove(listener_registration);
            if (!removed) {
                return std::unexpected(
                    wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, removed.error()));
            }
            listener_registration = {};
        }
        std::lock_guard lock(failure_mutex);
        service_status.accepting = false;
        return {};
    }

    Result<void> close_listener() {
        auto removed = unwatch_listener();
        if (!removed) {
            return removed;
        }
        accept_retry_at.reset();
        auto closed = listener.close();
        if (!closed) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::CloseSocket, closed.error()));
        }
        return {};
    }

    Result<void> pause_accepting() {
        auto removed = unwatch_listener();
        if (!removed) {
            return removed;
        }
        accept_retry_at = Clock::now() + accept_retry_delay;
        accept_retry_delay = (std::min)(accept_retry_delay * 2, config::shell_socket_accept_retry_max);
        return {};
    }

    Result<void> accept() {
        for (std::size_t operations = 0; operations < config::shell_socket_accept_budget; ++operations) {
#ifdef VINC_SHELL_TESTING
            auto incoming = test::accept(listener);
#else
            auto incoming = listener.try_accept();
#endif
            if (!incoming) {
                if (incoming.error().is(osal::ErrorCode::WouldBlock)) {
                    accept_retry_delay = config::shell_socket_accept_retry_initial;
                    return {};
                }
                record_failure(wrap_error(ErrorCode::SocketFailed, ErrorOperation::Accept, incoming.error()));
                if (incoming.error().is(osal::ErrorCode::ConnectionReset) ||
                    incoming.error().is(osal::ErrorCode::PeerClosed)) {
                    continue;
                }
                if (incoming.error().is(osal::ErrorCode::ResourceExhausted) ||
                    incoming.error().is(osal::ErrorCode::NetworkUnreachable)) {
                    return pause_accepting();
                }
                // An unclassified listener failure disables admission, not healthy clients or modules.
                return close_listener();
            }
            accept_retry_delay = config::shell_socket_accept_retry_initial;

            const auto count = std::count_if(sessions.begin(), sessions.end(), [](const auto& session) {
                return bool(session->socket);
            });
            if (count >= static_cast<std::ptrdiff_t>(options.max_connections) ||
                sessions.size() >= retained_session_limit() + config::shell_socket_resume_slots) {
                continue;
            }

            auto code = make_resume_code();
            if (!code) {
                return std::unexpected(code.error());
            }

            auto session = std::make_unique<Session>(next_session_id++, std::move(*code));
            session->provisional = sessions.size() >= retained_session_limit();
            session->socket = std::move(*incoming);
            (void)session->socket.set_option<osal::socket_options::NoDelay>(true);
            auto registered = register_session(*session);
            if (!registered) {
                return registered;
            }

            if (!session->provisional) {
                subscribe(*session);
            }

            reply(*session,
                  "VINC Socket Shell (Raw TCP). disconnect leaves this connection; exit shuts down "
                  "Vinc.\nResume code: " +
                      session->resume_code +
                      (session->provisional ? "\nSession storage full: only resume is available." : ""));
            sessions.push_back(std::move(session));
        }

        return {};
    }

    // Restore ownership, not just an IP address. Rotate the bearer code after use.
    Result<bool> resume(Session& new_session, const std::vector<std::string>& arguments,
                        Session*& receiving_session) {
        if (arguments.size() != 2) {
            reply(new_session, "Usage: resume <code>");
            return false;
        }

        if (new_session.job.valid() || scripts.active(new_session.id)) {
            reply(new_session, "Busy: this session already owns work");
            return false;
        }

        for (auto& retained_session : sessions) {
            if (retained_session.get() == &new_session || retained_session->socket ||
                retained_session->expired || retained_session->resume_code != arguments[1] ||
                Clock::now() - retained_session->disconnected_at >= options.disconnect_timeout) {
                continue;
            }

            auto replacement_code = make_resume_code();
            if (!replacement_code) {
                return std::unexpected(replacement_code.error());
            }
            (void)poller.remove(new_session.registration);
            new_session.output.close();
            retained_session->socket = std::move(new_session.socket);
            auto registered = register_session(*retained_session);
            if (!registered) {
                return std::unexpected(registered.error());
            }

            retained_session->resume_code = std::move(*replacement_code);
            retained_session->renderer = {};
            retained_session->last_send_progress = Clock::now();
            subscribe(*retained_session);
            new_session.expired = true;
            retained_session->previous_was_cr = new_session.previous_was_cr;
            receiving_session = retained_session.get();
            reply(*retained_session, "Session resumed. Disconnected output was not retained.\nResume code: " +
                                         retained_session->resume_code);
            return true;
        }

        reply(new_session, "Resume rejected: code unavailable or expired");
        return false;
    }

    // A handled command includes a rejected invocation. Do not fall through to routing.
    Result<bool> handle_session_command(Session& session, const std::vector<std::string>& arguments,
                                        Session*& receiving_session) {
        const auto& name = arguments.front();
        if (name == "resume") {
            auto resumed = resume(session, arguments, receiving_session);
            if (!resumed) {
                return std::unexpected(resumed.error());
            }

            return true;
        }

        if (session.provisional) {
            reply(session, "Session storage full: only resume is available");
            return true;
        }

        if (name == "disconnect") {
            if (arguments.size() != 1) {
                reply(session, "disconnect takes no arguments");
            } else {
                session.disconnect_requested = true;
            }

            return true;
        }

        if (name == "scancel") {
            if (arguments.size() != 1) {
                reply(session, "scancel takes no arguments");
            } else {
                reply(session, scripts.cancel() ? "Script cancellation requested" : "No script is running");
            }

            return true;
        }

        if (name == "sstatus") {
            if (arguments.size() != 1) {
                reply(session, "sstatus takes no arguments");
            } else if (scripts.active()) {
                reply(session, "Script running");
            } else {
                reply(session, "No script is running");
            }

            return true;
        }

        if (name == "output") {
            if (arguments.size() != 2 || (arguments[1] != "on" && arguments[1] != "off")) {
                reply(session, "Usage: output on|off");
                return true;
            }

            session.output.close();
            session.output_enabled = arguments[1] == "on";
            session.renderer = {};
            subscribe(session);
            reply(session, session.output_enabled ? "Module output on" : "Module output off");
            return true;
        }

        return false;
    }

    Result<void> command(Session& session, std::string command_line, Session*& receiving_session) {
        if (!is_valid_command_line(command_line)) {
            reply(session, "Rejected invalid UTF-8 or control character");
            return {};
        }

        auto parsed = parse_line(command_line);
        if (!parsed) {
            reply(session, "Parse error: " + vinc::error_to_string(parsed.error()));
            return {};
        }

        auto& arguments = parsed->arguments;
        if (arguments.empty()) {
            reply(session, "");
            return {};
        }

        const auto& name = arguments.front();
        auto handled = handle_session_command(session, arguments, receiving_session);
        if (!handled) {
            return std::unexpected(handled.error());
        }

        if (*handled) {
            return {};
        }

        if (shutdown_started.load()) {
            reply(session, "Framework is shutting down");
            return {};
        }

        if (name == "exit" && arguments.size() == 1) {
            reply(session, "Shutting down: waiting for all module processes...");
            shutdown_requested.store(true);
            return {};
        }

        if (session.job.valid()) {
            reply(session, "Busy: this session already has a command in flight");
            return {};
        }

        if (name == "srun") {
            if (arguments.size() < 2) {
                reply(session, "Usage: srun file.tcl [args...]");
                return {};
            }

            auto path = osal::path_from_utf8(arguments[1]);
            if (!path) {
                reply(session, error_to_string(wrap_error(ErrorCode::InvalidArgument,
                                                          ErrorOperation::StartScript, path.error())));
                return {};
            }

            auto result =
                scripts.start(session.id, std::move(*path), {arguments.begin() + 2, arguments.end()});
            reply(session, result ? "Script running. scancel cancels the global script."
                                  : vinc::error_to_string(result.error()));
            return {};
        }

        session.stop = {};
        session.job = std::async(
            std::launch::async, [this, arguments = std::move(arguments), stop = session.stop.get_token()] {
                try {
                    return router.execute(arguments, stop);
                } catch (const std::exception&) {
                    return Result<CommandReply>{
                        std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Execute})};
                } catch (...) {
                    return Result<CommandReply>{
                        std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Execute})};
                }
            });
        return {};
    }

    Result<void> receive(Session*& receiving_session) {
        auto& session = *receiving_session;
        std::array<std::byte, config::shell_socket_read_bytes> bytes{};
        auto received = session.socket.try_receive(bytes);
        if (!received) {
            if (!(received.error().is(vinc::osal::ErrorCode::WouldBlock))) {
                session.disconnect_requested = true;
            }

            return {};
        }

        if (received->eof) {
            session.disconnect_requested = true;
            return {};
        }

        for (std::size_t i = 0; i < received->bytes && receiving_session->socket &&
                                !receiving_session->disconnect_requested; ++i) {
            auto& current = *receiving_session;
            const auto character = static_cast<char>(bytes[i]);
            if (character == '\n' && current.previous_was_cr) {
                current.previous_was_cr = false;
                continue;
            }

            current.previous_was_cr = character == '\r';
            if (character == '\r' || character == '\n') {
                if (current.line_oversized) {
                    reply(current, "Rejected oversized command (maximum 4096 bytes)");
                } else {
                    auto handled = command(current, std::move(current.command_line), receiving_session);
                    if (!handled) {
                        return handled;
                    }
                }

                current.command_line.clear();
                current.line_oversized = false;
            } else if (!current.line_oversized) {
                if (current.command_line.size() == max_line_bytes) {
                    current.command_line.clear();
                    current.line_oversized = true;
                } else {
                    current.command_line += character;
                }
            }
        }

        return {};
    }

    void transmit(Session& session) {
        for (int operations = 0; operations < config::shell_socket_send_budget; ++operations) {
            if (session.sending_text.empty()) {
                if (session.replies.empty()) {
                    return;
                }

                session.sending_text = std::move(session.replies.front());
                session.replies.pop_front();
                session.queued_bytes -= session.sending_text.size();
                session.send_offset = 0;
            }

            auto sent = session.socket.try_send(
                std::as_bytes(std::span(session.sending_text)).subspan(session.send_offset));
            if (!sent) {
                if (!(sent.error().is(vinc::osal::ErrorCode::WouldBlock))) {
                    session.disconnect_requested = true;
                }

                return;
            }

            if (!*sent) {
                return;
            }

            session.send_offset += *sent;
            session.last_send_progress = Clock::now();
            if (session.send_offset == session.sending_text.size()) {
                session.sending_text.clear();
                session.send_offset = 0;
            }
        }
    }

    void collect(Session& session) {
        if (shutdown_started.load()) {
            session.stop.request_stop();
        }

        const bool can_collect_replies =
            session.queued_bytes + session.sending_text.size() <
                config::shell_socket_queue_bytes - config::shell_socket_reply_reserve_bytes &&
            session.replies.size() < config::shell_socket_collect_threshold_items;
        if (can_collect_replies && session.socket && !session.provisional) {
            const auto diagnostics = router.contract_notifications(session.contract_cursor);
            if (!diagnostics.empty()) {
                reply(session, diagnostics);
            }
        }
        collect_command_result(session, can_collect_replies);
        collect_script_result(session, can_collect_replies);
        collect_module_output(session);
        announce_shutdown(session);
    }

    void collect_command_result(Session& session, bool can_collect_replies) {
        if ((can_collect_replies || !session.socket) && session.job.valid() &&
            session.job.wait_for(0ms) == std::future_status::ready) {
            auto result = session.job.get();
            if (result && result->shutdown) {
                shutdown_requested.store(true);
            }

            if (session.socket) {
                const auto text = command_text(result);
                if (!text.empty()) {
                    reply(session, text);
                } else if (result) {
                    reply(session, "OK");
                } else {
                    reply(session, "Command failed");
                }
            }
        }
    }

    void collect_script_result(Session& session, bool can_collect_replies) {
        if (can_collect_replies || !session.socket) {
            auto delivery = scripts.take(session.id);
            if (session.socket) {
                if (!delivery.output.empty()) {
                    enqueue(session, std::move(delivery.output));
                }

                if (delivery.truncated) {
                    enqueue(session, "\n[Script output truncated]\n");
                }

                if (delivery.result) {
                    reply(session, script_text(*delivery.result));
                }
            }
        }
    }

    void collect_module_output(Session& session) {
        // Module output is already lossy in OutputHub. Consume only when no reliable
        // reply is waiting, preserving responsiveness and bounding network storage.
        if (session.socket && session.output_enabled && !session.output_drained &&
            session.sending_text.empty() && session.replies.empty()) {
            auto batch = session.output.take(config::shell_socket_output_batch_events);
            auto text = session.renderer.render(batch);
            if (draining && batch.events.size() < config::shell_socket_output_batch_events) {
                text += session.renderer.flush(Clock::now(), true);
                session.output_drained = true;
            }

            if (!text.empty()) {
                enqueue(session, std::move(text));
            }
        }
    }

    void announce_shutdown(Session& session) {
        if (draining && session.socket && !session.final_announced &&
            (!session.output_enabled || session.output_drained)) {
            reply(session, "All module processes exited; framework shutdown complete.");
            session.final_announced = true;
        }
    }

    void run() noexcept {
        try {
            auto result = loop();
            if (!result) {
                record_failure(result.error());
            }
        } catch (const std::system_error& failure) {
            record_failure(Error{ErrorCode::WorkerFailed, ErrorOperation::Run,
                                 osal::native_error(failure.code())});
        } catch (...) {
            record_failure(Error{ErrorCode::WorkerFailed, ErrorOperation::Run});
        }
        {
            std::lock_guard lock(failure_mutex);
            service_status.running = false;
            service_status.accepting = false;
        }

        if (listener) {
            // A broken poller must not leave a listening socket alive after its worker stops.
            if (listener_registration.value) {
                (void)poller.remove(listener_registration);
                listener_registration = {};
            }
            auto closed = listener.close();
            if (!closed) {
                record_failure(wrap_error(ErrorCode::SocketFailed, ErrorOperation::CloseSocket, closed.error()));
            }
        }
        for (auto& session : sessions) {
            detach(*session);
            session->stop.request_stop();
            scripts.cancel(session->id);
            scripts.forget(session->id);
        }

        // Retain ownership until native callbacks return. A network failure never stops modules.
        sessions.clear();
    }

    Result<void> update_watch(Session& session) {
        auto interests = osal::SocketEvents::None;
        if (!shutdown_started.load() &&
            session.queued_bytes + session.sending_text.size() <
                config::shell_socket_queue_bytes - config::shell_socket_reply_reserve_bytes) {
            interests = osal::SocketEvents::Readable;
        }

        if (!session.sending_text.empty() || !session.replies.empty()) {
            interests = interests | osal::SocketEvents::Writable;
        }

        // OSAL deliberately rejects an empty watch. Unregister idle
        // shutdown connections and add a new watch if a final reply arrives.
        if (interests == osal::SocketEvents::None) {
            if (session.registration.value) {
                auto removed = poller.remove(session.registration);
                if (!removed) {
                    return std::unexpected(
                        wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, removed.error()));
                }
            }

            session.registration = {};
        } else if (!session.registration.value) {
            auto added = poller.add(session.socket, interests, session.id);
            if (!added) {
                return std::unexpected(
                    wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, added.error()));
            }

            session.registration = *added;
        } else {
            auto modified = poller.modify(session.registration, interests);
            if (!modified) {
                return std::unexpected(
                    wrap_error(ErrorCode::SocketFailed, ErrorOperation::RegisterSocket, modified.error()));
            }
        }

        return {};
    }

    Result<bool> advance_session(Session& session) {
        bool pending = false;

        collect(session);
        if (session.socket) {
            if ((!session.sending_text.empty() || !session.replies.empty()) &&
                Clock::now() - session.last_send_progress >= options.send_timeout) {
                session.disconnect_requested = true;
            }

            if (session.disconnect_requested) {
                detach(session);
            } else {
                auto watched = update_watch(session);
                if (!watched) {
                    return std::unexpected(watched.error());
                }

                pending |= !session.sending_text.empty() || !session.replies.empty();
            }
        }

        if (!session.socket && !session.expired &&
            Clock::now() - session.disconnected_at >= options.disconnect_timeout) {
            session.expired = true;
            scripts.cancel(session.id);
            scripts.forget(session.id);
            session.stop.request_stop();
        }

        pending |= session.job.valid();
        pending |= finish_requested.load() && session.socket && !session.final_announced;
        return pending;
    }

    // Re-find by registration each time: a resume command can move a socket
    // to an older session while handling an earlier event from this batch.
    Result<void> handle_ready_event(const osal::SocketReadyEvent& event) {
        if (listener && event.registration == listener_registration) {
            if (osal::has_event(event.events, osal::SocketEvents::Error) ||
                osal::has_event(event.events, osal::SocketEvents::Hangup)) {
                record_failure(Error{ErrorCode::SocketFailed, ErrorOperation::Accept});
                return close_listener();
            }
            if (!shutdown_started.load()) {
                return accept();
            }

            return {};
        }

        const auto it = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) {
            return session->socket && session->registration == event.registration;
        });
        if (it == sessions.end()) {
            return {};
        }

        auto* session = it->get();
        if (osal::has_event(event.events, osal::SocketEvents::Readable)) {
            auto received = receive(session);
            if (!received) {
                return received;
            }
        }

        if (session->socket && !session->disconnect_requested &&
            osal::has_event(event.events, osal::SocketEvents::Writable)) {
            transmit(*session);
        }

        if (osal::has_event(event.events, osal::SocketEvents::Error) ||
            osal::has_event(event.events, osal::SocketEvents::Hangup)) {
            session->disconnect_requested = true;
        }

        return {};
    }

    Result<void> loop() {
        std::array<osal::SocketReadyEvent, config::shell_socket_ready_events> ready{};
        std::optional<vinc::Deadline> final_deadline;
        for (;;) {
            if (abort_requested.load()) {
                break;
            }

            if (shutdown_started.load() && listener) {
                auto closed = close_listener();
                if (!closed) {
                    return closed;
                }
            } else if (listener && accept_retry_at && Clock::now() >= *accept_retry_at) {
                auto registered = register_listener();
                if (!registered) {
                    return registered;
                }
                accept_retry_at.reset();
            }

            if (finish_requested.load() && !final_deadline &&
                std::none_of(sessions.begin(), sessions.end(), [](const auto& session) {
                    return session->job.valid();
                })) {
                draining = true;
                final_deadline = Clock::now() + config::shell_socket_final_drain_timeout;
            }

            bool pending = false;
            for (auto& session : sessions) {
                auto advanced = advance_session(*session);
                if (!advanced) {
                    return std::unexpected(advanced.error());
                }

                pending |= *advanced;
            }

            std::erase_if(sessions, [&](const auto& session) {
                if (!session->expired || session->job.valid()) {
                    return false;
                }

                scripts.forget(session->id);
                return true;
            });
            if (final_deadline && (!pending || Clock::now() >= *final_deadline)) {
                break;
            }

#ifdef VINC_SHELL_TESTING
            auto events = test::wait(poller, ready, Clock::now() + config::shell_socket_poll_interval);
#else
            auto events = poller.wait_until(ready, Clock::now() + config::shell_socket_poll_interval);
#endif
            if (!events) {
                if (events.error().is(vinc::osal::ErrorCode::Timeout)) {
                    continue;
                }

                return std::unexpected(
                    wrap_error(ErrorCode::SocketFailed, ErrorOperation::Poll, events.error()));
            }

            for (std::size_t i = 0; i < *events; ++i) {
                auto handled = handle_ready_event(ready[i]);
                if (!handled) {
                    return handled;
                }
            }
        }

        return {};
    }
};

SocketShell::SocketShell(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

Result<std::unique_ptr<SocketShell>> SocketShell::create(ModuleManager& manager, ScriptService& scripts,
                                                         SocketShellOptions options) {
    try {
        auto poller = osal::SocketPoller::create(config::shell_socket_poller_capacity);
        if (!poller) {
            return std::unexpected(
                wrap_error(ErrorCode::SocketFailed, ErrorOperation::CreatePoller, poller.error()));
        }

        auto impl = std::make_unique<Impl>(manager, scripts, std::move(options), std::move(*poller));
        auto initialized = impl->initialize();
        if (!initialized) {
            return std::unexpected(initialized.error());
        }

        auto owner = std::unique_ptr<SocketShell>(new SocketShell(std::move(impl)));
        owner->impl_->start_worker();
        return owner;
    } catch (const std::system_error& failure) {
        return std::unexpected(
            Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker, osal::native_error(failure.code())});
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Create});
    }
}

SocketShell::~SocketShell() = default;

osal::IpEndpoint SocketShell::endpoint() const {
    return impl_->bound_endpoint;
}

bool SocketShell::shutdown_requested() const noexcept {
    return impl_->shutdown_requested.load();
}

SocketShellStatus SocketShell::status() const {
    std::lock_guard lock(impl_->failure_mutex);
    return impl_->service_status;
}

std::optional<Error> SocketShell::failure() const {
    std::lock_guard lock(impl_->failure_mutex);
    return impl_->service_status.last_failure;
}

void SocketShell::begin_shutdown() {
    impl_->shutdown_started.store(true);
}

void SocketShell::finish_shutdown() {
    impl_->shutdown_started.store(true);
    impl_->finish_requested.store(true);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}
}
