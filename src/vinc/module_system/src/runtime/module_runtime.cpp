#include <vinc/log/detail/transport.hpp>
#include <vinc/module/detail/module_locks.hpp>
#include <vinc/module/detail/module_params.hpp>
#include <vinc/param/detail/client_access.hpp>
#include <vinc/param/detail/protocol.hpp>
#include <vinc/module/detail/context_runtime.hpp>
// Child bootstrap and hook dispatcher. A separate watchdog receives control frames
// while application hooks run serially, so a blocked hook cannot disable parent-loss detection.

#include <vinc/time.hpp>
#include <vinc/module/detail/plugin_loader.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/module/detail/runtime_services.hpp>
#include <vinc/config.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <vinc/module/base_module.hpp>
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/control/channel.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/message_bus/detail/runtime.hpp>
#include <atomic>
#include <bit>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef VINC_MODULE_TESTING
#include "fault_hooks.hpp"
#endif

namespace vinc {
class detail::ModuleMessageBusAccess {
public:
    static void attach(detail::ModuleObject& module, std::shared_ptr<message_bus::Client> client,
                       ContextRuntime& runtime, param::detail::Access& params, log::detail::Access* logs) {
        module.context.client_ = std::move(client);
        module.context.runtime_ = &runtime;
        param::detail::ClientAccess::bind(module.context.param, params);
        if (logs) {
            log::detail::ClientAccess::bind(module.context.log, *logs);
        }
    }

    static void quiesce(detail::ModuleObject& module) {
        // on_stop must join application workers first. Supervision bounds a stuck pump.
        while (module.context.pumping_.test()) {
            module.context.pumping_.wait(true);
        }
    }

    static void identify(detail::ModuleObject& module, MID mid) {
        module.mid_ = mid;
    }

    static void name(detail::ModuleObject& module, std::string name) {
        module.module_name_ = std::move(name);
    }
};

namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;

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
constexpr std::size_t central_session_limit = 2;
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
    bool debug;
};

// Shared only by the watchdog and main dispatcher. The mutex protects every
// non-atomic field; finished keeps its existing sequentially consistent ordering.
struct RuntimeInbox {
    log::detail::Host* logs = nullptr;
    detail::ModuleParams* params = nullptr;
    detail::ModuleLocks* locks = nullptr; // Host owner outlives inbox/watchdog/hooks.
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<control::Frame> requests;
    std::atomic<bool> stopping{false};
    std::optional<Error> terminal_failure;
    Deadline local_stop_deadline = (Deadline::max)();
    std::chrono::milliseconds stop_grace{};
    std::uint16_t pending_completion = 0;
    bool completion_acknowledged = false;
    bool debug_gate_open = false;
    bool debug_released = false;
    std::optional<control::Frame> registration;
    std::uint64_t next_fault_request = detail::name_registration_request + 1;
    std::uint64_t pending_fault_request = 0;
    bool fault_acknowledged = false;
    std::atomic<bool> finished{false};
};

Result<Bootstrap> parse_bootstrap(const std::vector<std::string>& args) {
    if (args.size() < detail::bootstrap::argument_count ||
        args[detail::bootstrap::marker] != detail::bootstrap::version_marker) {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }

    const auto generation = parse_number(args[detail::bootstrap::generation]);
    const auto interval = parse_number(args[detail::bootstrap::heartbeat_interval]);
    const auto timeout = parse_number(args[detail::bootstrap::heartbeat_timeout]);
    const auto startup = parse_number(args[detail::bootstrap::startup_timeout]);
    const auto grace = parse_number(args[detail::bootstrap::stop_timeout]);
    if (!generation || !interval || !timeout || !startup || !grace) {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }

    const bool invalid_heartbeat =
        !valid_mid(*generation) || !*interval || *interval >= *timeout || *timeout > maximum_bootstrap_timeout_ms;
    if (invalid_heartbeat) {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }

    const bool invalid_startup_timeout = !*startup || *startup > maximum_bootstrap_timeout_ms;
    if (invalid_startup_timeout) {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }

    const bool invalid_stop_timeout = !*grace || *grace > maximum_stop_grace_ms;
    if (invalid_stop_timeout || args[detail::bootstrap::base_name].empty()) {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }

    const auto& debug = args[detail::bootstrap::debug];
    if (debug != "0" && debug != "1") {
        return std::unexpected(Error{ErrorCode::InvalidBootstrap, ErrorOperation::Bootstrap});
    }
    return Bootstrap{*generation, *interval, *timeout, *startup, *grace, debug == "1"};
}

// Socket ownership transfers to Service only after a successful attach. The caller
// keeps Service alive through watchdog shutdown; failed setup releases local resources.
Result<std::shared_ptr<control::Session>> connect_to_central(control::Service& service,
                                                             const std::vector<std::string>& args,
                                                             const Bootstrap& bootstrap,
                                                             control::Options& options) {
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket) {
        return std::unexpected(
            wrap_error(ErrorCode::EndpointFailed, ErrorOperation::Connect, socket.error()));
    }

    auto connected =
        socket->connect_until(osal::LocalEndpoint{args[detail::bootstrap::endpoint]},
                              control::Clock::now() + std::chrono::milliseconds(bootstrap.startup));
    if (!connected) {
        return std::unexpected(
            wrap_error(ErrorCode::EndpointFailed, ErrorOperation::Connect, connected.error()));
    }

    options.heartbeat_timeout_enabled = !bootstrap.debug;
    options.role = control::Role::Module;
    options.token = args[detail::bootstrap::token];
    options.generation = bootstrap.generation;
    options.heartbeat_interval = std::chrono::milliseconds(bootstrap.interval);
    options.heartbeat_timeout = std::chrono::milliseconds(bootstrap.timeout);
    options.handshake_timeout = std::chrono::milliseconds(bootstrap.startup);
    auto attached = service.attach(std::move(*socket), options);
    if (!attached) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::AttachChannel, attached.error()));
    }

    return *attached;
}

// Only the watchdog receives here. Preserve its typed fault when the channel is
// usable; the stop deadline still takes precedence over diagnostic delivery.
[[noreturn]] void watchdog_failure(const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox,
                                   const Error& error, int exit_code, Deadline stop_deadline,
                                   std::uint32_t role = detail::primary_failure) noexcept {
    if (inbox.locks) {
        inbox.locks->stop();
        inbox.params->stop();
    }
    try {
        control::Frame report{control::Kind::Request, detail::lifecycle, detail::report_failure};
        {
            std::lock_guard lock(inbox.mutex);
            report.request = inbox.next_fault_request++;
            control::put_u32(report.payload, role);
            const auto encoded = detail::encode_error(error);
            report.payload.insert(report.payload.end(), encoded.begin(), encoded.end());
            if (!session->send(report)) {
                std::_Exit(exit_code);
            }
        }
        const auto deadline =
            (std::min)(stop_deadline, control::Clock::now() + config::module_error_report_timeout);
        while (control::Clock::now() < deadline) {
            auto response = session->receive_until(deadline);
            if (!response) {
                break;
            }
            if (response->kind == control::Kind::Response && response->domain == detail::lifecycle &&
                response->operation == detail::report_failure && response->request == report.request &&
                response->payload.empty()) {
                break;
            }
        }
    } catch (...) {
        // A broken channel or exhausted allocator cannot guarantee a final report.
    }
    std::_Exit(exit_code);
}

// Runs independently of constructors, hooks and destructors. Session loss and an
// expired stop grace use _Exit because application code may be stuck. No user hook runs here.
void watch_central(const std::shared_ptr<control::Session>& session, message_bus::Client& message_bus_client,
                   RuntimeInbox& inbox, std::chrono::milliseconds stop_grace, std::stop_token token) {
    auto stop_deadline = (vinc::Deadline::max)();
    try {
        while (!token.stop_requested()) {
            std::optional<Error> terminal;
            {
                std::lock_guard lock(inbox.mutex);
                stop_deadline = (std::min)(stop_deadline, inbox.local_stop_deadline);
                terminal = inbox.terminal_failure;
            }
            if (terminal) {
                watchdog_failure(session, inbox, *terminal, child_exit::application_exception, stop_deadline);
            }
            if (inbox.logs) {
                inbox.logs->poll();
            }
            const auto poll_deadline =
                (std::min)(stop_deadline, control::Clock::now() + config::module_runtime_poll_interval);
            auto frame = session->receive_until(poll_deadline);
            if (inbox.finished.load()) {
                return;
            }

            if (control::Clock::now() >= stop_deadline) {
                watchdog_failure(session, inbox, Error{ErrorCode::StopDeadlineExpired, ErrorOperation::Stop},
                                 child_exit::stop_deadline_expired, stop_deadline, detail::cleanup_failure);
            }
            if (!frame) {
                if (frame.error().is(control::ErrorCode::Timeout) &&
                    session->state() != control::State::Closed) {
                    continue;
                }

                watchdog_failure(session, inbox,
                                 wrap_error(ErrorCode::CentralLost, ErrorOperation::Receive, frame.error()),
                                 child_exit::central_lost, stop_deadline);
            }
            if (frame->kind == control::Kind::Response && frame->domain == detail::lifecycle &&
                frame->operation == detail::attach_lock) {
                auto received = inbox.locks->receive(std::move(*frame));
                if (!received) {
                    watchdog_failure(session, inbox, received.error(), child_exit::central_lost, stop_deadline);
                }
                continue;
            }
            if (frame->kind == control::Kind::Response && frame->domain == detail::lifecycle &&
                (frame->operation == detail::finishing || frame->operation == detail::completed)) {
                std::unique_lock lock(inbox.mutex);
                if (frame->operation != inbox.pending_completion ||
                    frame->request != detail::completion_request || !frame->payload.empty() ||
                    inbox.completion_acknowledged) {
                    lock.unlock();
                    watchdog_failure(session, inbox, Error{ErrorCode::Protocol, ErrorOperation::Finish},
                                     child_exit::central_lost, stop_deadline);
                }
                inbox.completion_acknowledged = true;
                inbox.changed.notify_all();
            } else if (frame->kind == control::Kind::Response && frame->domain == detail::lifecycle &&
                frame->operation == detail::register_name &&
                frame->request == detail::name_registration_request) {
                std::unique_lock lock(inbox.mutex);
                if (inbox.registration) {
                    lock.unlock();
                    watchdog_failure(session, inbox, Error{ErrorCode::Protocol, ErrorOperation::RegisterName},
                                     child_exit::central_lost, stop_deadline);
                }

                inbox.registration = std::move(*frame);
                inbox.changed.notify_all();
            } else if (frame->kind == control::Kind::Response && frame->domain == detail::lifecycle &&
                       frame->operation == detail::report_failure) {
                std::unique_lock lock(inbox.mutex);
                // A previous report may have exhausted its bounded wait. Its late acknowledgement
                // does not settle the currently pending report and is safe to discard.
                if (frame->request > detail::name_registration_request &&
                    frame->request < inbox.pending_fault_request && frame->payload.empty()) {
                    continue;
                }
                if (frame->request != inbox.pending_fault_request || !frame->payload.empty() ||
                    inbox.fault_acknowledged) {
                    lock.unlock();
                    watchdog_failure(session, inbox,
                                     Error{ErrorCode::Protocol, ErrorOperation::ReportFailure},
                                     child_exit::central_lost, stop_deadline);
                }
                inbox.fault_acknowledged = true;
                inbox.changed.notify_all();
            } else if (frame->kind == control::Kind::Event && frame->domain == detail::lifecycle &&
                       frame->operation == detail::debug_continue) {
                std::unique_lock lock(inbox.mutex);
                if (!inbox.debug_gate_open || inbox.debug_released || !frame->payload.empty()) {
                    lock.unlock();
                    watchdog_failure(session, inbox, Error{ErrorCode::Protocol, ErrorOperation::ContinueDebug},
                                     child_exit::central_lost, stop_deadline);
                }
                inbox.debug_released = true;
                inbox.changed.notify_all();
            } else if (frame->domain == param::detail::domain) {
                auto received = inbox.params->receive(std::move(*frame));
                if (!received) {
                    watchdog_failure(session, inbox,
                        wrap_error(ErrorCode::ParamFailed, ErrorOperation::Receive, received.error()),
                        child_exit::central_lost, stop_deadline);
                }
                inbox.changed.notify_all();
            } else if (frame->domain == message_bus::detail::message_bus_domain) {
                auto received =
                    message_bus::detail::RuntimeAccess::receive(message_bus_client, std::move(*frame));
                if (!received) {
                    watchdog_failure(
                        session, inbox,
                        wrap_error(ErrorCode::BusFailed, ErrorOperation::Receive, received.error()),
                        child_exit::central_lost, stop_deadline);
                }
            } else if (frame->kind == control::Kind::Event && frame->domain == detail::lifecycle &&
                       frame->operation == detail::stop) {
                if (!frame->payload.empty()) {
                    watchdog_failure(session, inbox, Error{ErrorCode::Protocol, ErrorOperation::Stop},
                                     child_exit::invalid_stop_message, stop_deadline);
                }
                inbox.locks->stop();
                inbox.params->stop();
                std::lock_guard lock(inbox.mutex);
                if (!inbox.stopping) {
                    inbox.stopping = true;
                    stop_deadline = control::Clock::now() + stop_grace;
                }
                inbox.changed.notify_all();
            } else if (frame->kind == control::Kind::Request) {
                std::unique_lock lock(inbox.mutex);
                if (inbox.requests.size() >= pending_request_limit) {
                    lock.unlock();
                    watchdog_failure(session, inbox,
                                     Error{ErrorCode::CapacityExceeded, ErrorOperation::Receive},
                                     child_exit::central_lost, stop_deadline);
                }

                inbox.requests.push_back(std::move(*frame));
                inbox.changed.notify_all();
            } else {
                watchdog_failure(session, inbox, Error{ErrorCode::Protocol, ErrorOperation::Receive},
                                 child_exit::central_lost, stop_deadline);
            }
        }
    } catch (...) {
        watchdog_failure(session, inbox, Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise},
                         child_exit::watchdog_exception, stop_deadline);
    }
}

// Wait before executing any Plugin code. Stop wakes the gate without loading the library.
// The existing receiver owns protocol progress; no polling or extra thread is needed here.
Result<bool> wait_debug_gate(const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox) {
    std::unique_lock lock(inbox.mutex);
    inbox.debug_gate_open = true;
    auto sent = session->send({control::Kind::Event, detail::lifecycle, detail::debug_waiting});
    if (!sent) {
        inbox.debug_gate_open = false;
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::WaitDebugReady, sent.error()));
    }
    inbox.changed.wait(lock, [&] { return inbox.debug_released || inbox.stopping; });
    inbox.debug_gate_open = false;
    return !inbox.stopping;
}

// User code is an exception boundary. Expected hook failures keep their original cause.
template <class Function>
auto invoke_hook(ErrorOperation operation, Function&& function) -> decltype(function()) {
    try {
        auto result = function();
        if (!result) {
            return std::unexpected(wrap_error(ErrorCode::HookFailed, operation, result.error()));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, operation});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::HookException, operation});
    }
}

Result<void> report_failure(const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox,
                            const Error& error, std::uint32_t role) {
#ifdef VINC_MODULE_TESTING
    module_system::test::checkpoint(module_system::test::Point::ReportFailure);
#endif
    std::unique_lock lock(inbox.mutex);
    control::Frame report{control::Kind::Request, detail::lifecycle, detail::report_failure,
                          inbox.next_fault_request++};
    inbox.pending_fault_request = report.request;
    inbox.fault_acknowledged = false;
    control::put_u32(report.payload, role);
    const auto encoded = detail::encode_error(error);
    report.payload.insert(report.payload.end(), encoded.begin(), encoded.end());
    auto sent = session->send(std::move(report));
    if (!sent) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::ReportFailure, sent.error()));
    }
    // send() only enqueues. Wait for central storage before closing a healthy channel.
    const auto deadline = control::Clock::now() + config::module_error_report_timeout;
    if (!inbox.changed.wait_until(lock, deadline, [&] {
            return inbox.fault_acknowledged;
        })) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::ReportFailure});
    }
    return {};
}

Result<void> register_module_name(detail::ModuleObject& module, std::vector<std::string>& startup_args,
                                  const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox,
                                  std::chrono::milliseconds timeout) {
    auto proposed = invoke_hook(ErrorOperation::NameHook, [&] {
        return module.on_name(startup_args);
    });
    if (!proposed) {
        return std::unexpected(proposed.error());
    }
    control::Frame claim{control::Kind::Request, detail::lifecycle, detail::register_name,
                         detail::name_registration_request};
    auto encoded = control::put_string(claim.payload, *proposed);
    if (!encoded) {
        return std::unexpected(
            wrap_error(ErrorCode::InvalidName, ErrorOperation::RegisterName, encoded.error()));
    }
    auto sent = session->send(std::move(claim));
    if (!sent) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::RegisterName, sent.error()));
    }

    std::unique_lock lock(inbox.mutex);
    const bool received = inbox.changed.wait_until(lock, control::Clock::now() + timeout, [&] {
        return inbox.registration.has_value() || inbox.stopping;
    });
    if (!received) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::RegisterName});
    }
    if (!inbox.registration) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::RegisterName});
    }
    auto payload = detail::decode_response(inbox.registration->payload);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    control::Reader reader(*payload);
    auto name = reader.string();
    if (!reader.done() || name != *proposed) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::RegisterName});
    }
    detail::ModuleMessageBusAccess::name(module, std::move(name));
    return {};
}

Result<control::Bytes> execute_request(detail::ModuleObject& module, const control::Frame& request) {
    if (request.domain != detail::commands ||
        (request.operation != detail::execute && request.operation != detail::complete)) {
        return std::unexpected(Error{ErrorCode::UnsupportedOperation, ErrorOperation::Receive});
    }
    auto args = control::decode_arguments(request.payload);
    if (!args) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Receive, args.error()));
    }
    control::Bytes payload;
    control::put_u32(payload, detail::result_success);
    if (request.operation == detail::execute) {
        auto result = invoke_hook(ErrorOperation::CommandHook, [&] {
            return module.module_interface(std::move(*args));
        });
        if (!result) {
            return std::unexpected(result.error());
        }
        control::put_u32(payload, std::bit_cast<std::uint32_t>(*result));
    } else {
        auto result = invoke_hook(ErrorOperation::CompleteHook, [&] {
            return module.module_autoComplete(std::move(*args));
        });
        if (!result) {
            return std::unexpected(result.error());
        }
        auto encoded = control::encode_arguments(*result);
        if (!encoded) {
            return std::unexpected(
                wrap_error(ErrorCode::InvalidArgument, ErrorOperation::CompleteHook, encoded.error()));
        }
        if (encoded->size() > control::max_payload - detail::result_tag_bytes) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::CompleteHook});
        }
        payload.insert(payload.end(), encoded->begin(), encoded->end());
    }
    return payload;
}

Result<void> dispatch_request(detail::ModuleObject& module, const control::Frame& request,
                              const std::shared_ptr<control::Session>& session) {
    auto payload = execute_request(module, request);
    control::Frame response{control::Kind::Response, request.domain, request.operation, request.request};
    response.payload = payload ? std::move(*payload) : detail::encode_failure(payload.error());
    auto sent = session->send(std::move(response));
    if (!sent) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Send, sent.error()));
    }
    return {};
}

// The Module owns Context; this adapter owns no thread or transport. RuntimeGuard
// outlives it and the Plugin remains loaded until every callback wrapper is destroyed.
class HostContext final : public detail::ContextRuntime {
    detail::ModuleObject& module_;
    std::shared_ptr<control::Session> session_;
    RuntimeInbox& inbox_;
public:
    HostContext(detail::ModuleObject& module, std::shared_ptr<control::Session> session, RuntimeInbox& inbox)
        : module_(module), session_(std::move(session)), inbox_(inbox) {
    }
    bool living() const noexcept override {
        return !inbox_.stopping.load();
    }
    Result<std::uint64_t> commands() override {
        std::deque<control::Frame> requests;
        {
            std::lock_guard lock(inbox_.mutex);
            requests.swap(inbox_.requests);
        }
        std::uint64_t count = 0;
        for (const auto& request : requests) {
            if (!living()) {
                break;
            }
            auto dispatched = dispatch_request(module_, request, session_);
            if (!dispatched) {
                return std::unexpected(dispatched.error());
            }
            ++count;
        }
        return count;
    }
    Result<std::unique_ptr<detail::ContextLock>> lock(std::uint64_t id, Deadline deadline) override {
        return inbox_.locks->lock(id, deadline);
    }
    void wait() override {
        std::unique_lock lock(inbox_.mutex);
        inbox_.changed.wait_for(lock, config::module_pump_idle_interval, [&] {
            return inbox_.stopping.load() || !inbox_.requests.empty();
        });
    }
    void fail(const Error& error) override {
        inbox_.locks->stop();
        inbox_.params->stop();
        std::lock_guard lock(inbox_.mutex);
        if (!inbox_.terminal_failure) {
            inbox_.terminal_failure = error;
        }
        inbox_.stopping.store(true);
        inbox_.local_stop_deadline = (std::min)(inbox_.local_stop_deadline,
            control::Clock::now() + inbox_.stop_grace);
        inbox_.changed.notify_all();
    }
};

Result<void> completion_exchange(const std::shared_ptr<control::Session>& session,
                                 RuntimeInbox& inbox, std::uint16_t operation) {
    std::unique_lock lock(inbox.mutex);
    inbox.pending_completion = operation;
    inbox.completion_acknowledged = false;
    auto sent = session->send({control::Kind::Request, detail::lifecycle, operation, detail::completion_request});
    if (!sent) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Finish, sent.error()));
    }
    if (!inbox.changed.wait_until(lock, inbox.local_stop_deadline, [&] {
            return inbox.completion_acknowledged;
        })) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::Finish});
    }
    return {};
}

// Own the watchdog shutdown order even when allocation or a framework operation throws.
struct RuntimeGuard {
    RuntimeInbox& inbox;
    std::shared_ptr<control::Session> session;
    message_bus::Client& client;
    std::jthread watchdog;

    ~RuntimeGuard() {
        inbox.locks->stop();
        inbox.params->stop();
        inbox.finished.store(true);
        message_bus::detail::RuntimeAccess::shutdown(client);
        session->close();
        watchdog.request_stop();
        if (watchdog.joinable()) {
            watchdog.join();
        }
    }
};

Result<void> start_and_dispatch(detail::ModuleObject& module, std::vector<std::string>& args,
                                const std::shared_ptr<control::Session>& session) {
    auto started = invoke_hook(ErrorOperation::StartHook, [&] {
        return module.on_start(args);
    });
    if (!started) {
        return std::unexpected(started.error());
    }
    control::Frame ready{control::Kind::Event, detail::lifecycle, detail::ready};
#ifdef VINC_MODULE_TESTING
    module_system::test::checkpoint(module_system::test::Point::Ready);
#endif
    auto encoded = control::put_string(ready.payload, module.get_module_name());
    if (!encoded) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Ready, encoded.error()));
    }
    auto sent = session->send(std::move(ready));
    if (!sent) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Ready, sent.error()));
    }
    return invoke_hook(ErrorOperation::MainHook, [&] {
        return module.main();
    });
}

Result<void> run_started_hooks(detail::ModuleObject& module, std::vector<std::string>& startup_args,
                               const std::shared_ptr<control::Session>& session, RuntimeInbox& inbox) {
    Result<void> running;
    try {
        running = start_and_dispatch(module, startup_args, session);
    } catch (const std::bad_alloc&) {
        running = std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Supervise});
    } catch (const std::system_error& error) {
        running = std::unexpected(Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise,
                                        osal::native_error(error.code())});
    } catch (...) {
        running = std::unexpected(Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise});
    }
    // Diagnostic delivery is best effort. It must not bypass the stop hook;
    // the independent watchdog still enforces the existing stop deadline.
    const auto report = [&](const Error& error, std::uint32_t role) noexcept {
        try {
            (void)report_failure(session, inbox, error, role);
        } catch (...) {
            // Keep the original failure; reporting cannot replace its cause.
        }
    };
    bool completed_locally = false;
    {
        std::lock_guard lock(inbox.mutex);
        completed_locally = running.has_value() && !inbox.stopping.load();
        inbox.stopping.store(true);
        inbox.local_stop_deadline = (std::min)(inbox.local_stop_deadline,
            control::Clock::now() + inbox.stop_grace);
        if (inbox.terminal_failure) {
            running = std::unexpected(*inbox.terminal_failure);
        }
        inbox.changed.notify_all();
    }
    inbox.locks->stop();
    inbox.params->stop();
    if (completed_locally && running) {
        running = completion_exchange(session, inbox, detail::finishing);
    }
    if (!running) {
        report(running.error(), detail::primary_failure);
    }
    auto stopped = invoke_hook(ErrorOperation::StopHook, [&] {
        return module.on_stop();
    });
    if (!stopped) {
        report(stopped.error(), detail::cleanup_failure);
    }
    detail::ModuleMessageBusAccess::quiesce(module);
    {
        std::lock_guard lock(inbox.mutex);
        if (inbox.terminal_failure) {
            running = std::unexpected(*inbox.terminal_failure);
        }
    }
    if (!running) {
        return std::unexpected(running.error());
    }
    return stopped;
}

Result<void> run_hooks(const std::vector<std::string>& args, const Bootstrap& bootstrap,
                       const std::shared_ptr<control::Session>& session,
                       const std::shared_ptr<message_bus::Client>& client, RuntimeInbox& inbox,
                       const detail::LoadedPlugin& plugin) {
    auto module = invoke_hook(ErrorOperation::ConstructModule, [&]() -> Result<detail::PluginModule> {
        auto instance = plugin.create(args[detail::bootstrap::base_name]);
        if (!instance) {
            return std::unexpected(instance.error());
        }
        if (!(*instance) || (*instance)->get_module_name() != args[detail::bootstrap::base_name]) {
            return std::unexpected(Error{ErrorCode::InvalidFactory, ErrorOperation::ConstructModule});
        }
        return instance;
    });
    if (!module) {
        return std::unexpected(module.error());
    }
    detail::ModuleMessageBusAccess::identify(**module, bootstrap.generation);
    std::vector<std::string> startup_args(args.begin() + detail::bootstrap::argument_count, args.end());
    auto registered = register_module_name(**module, startup_args, session, inbox,
                                           std::chrono::milliseconds(bootstrap.startup));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    HostContext context_runtime(**module, session, inbox);
    inbox.params->identify((*module)->get_module_name());
    detail::ModuleMessageBusAccess::attach(**module, client, context_runtime, *inbox.params, inbox.logs);
    auto result = run_started_hooks(**module, startup_args, session, inbox);
    // Destroy user state and callback closures while both Host adapter and Plugin exist.
    module->reset();
    if (!result) {
        return result;
    }
    bool needs_completion = false;
    {
        std::lock_guard lock(inbox.mutex);
        needs_completion = inbox.pending_completion == detail::finishing;
    }
    if (needs_completion) {
        return completion_exchange(session, inbox, detail::completed);
    }
    return {};
}

int exit_with_error(int status, const Error& error) noexcept {
    try {
        std::cerr << error_to_string(error) << '\n';
    } catch (...) {
        // Numeric OS exit remains observable even if formatting/output allocation fails.
    }
    return status;
}

} // namespace

// Framework bootstrap arguments precede application arguments; use ModuleManager to launch.
int run_module_process(const std::vector<std::string>& args) {
    try {
        auto bootstrap = parse_bootstrap(args);
        if (!bootstrap) {
            return exit_with_error(child_exit::invalid_bootstrap, bootstrap.error());
        }
        auto fault_storage = detail::ContractReportStorage::open(args[detail::bootstrap::fault_storage], bootstrap->generation);
        if (!fault_storage) {
            return exit_with_error(child_exit::invalid_bootstrap, fault_storage.error());
        }
        detail::HostRuntimeServices runtime_services(**fault_storage);
        auto bound = runtime_services.bind_host();
        if (!bound) {
            return exit_with_error(child_exit::invalid_bootstrap, bound.error());
        }

        std::optional<detail::LoadedPlugin> plugin;
        auto service = control::Service::create(central_session_limit);
        if (!service) {
            return exit_with_error(
                child_exit::connection_failed,
                wrap_error(ErrorCode::ServiceFailed, ErrorOperation::Create, service.error()));
        }
        control::Options options;
        auto connected = connect_to_central(**service, args, *bootstrap, options);
        if (!connected) {
            return exit_with_error(child_exit::connection_failed, connected.error());
        }
        auto session = *connected;
        auto opened = session->wait_open(control::Clock::now() + options.handshake_timeout);
        if (!opened) {
            return exit_with_error(
                child_exit::handshake_failed,
                wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, opened.error()));
        }
        auto client = message_bus::detail::RuntimeAccess::create(session);
        if (!client) {
            // Before the dispatcher exists, receive this acknowledgement synchronously.
            const auto error =
                wrap_error(ErrorCode::BusFailed, ErrorOperation::AccessMessageBus, client.error());
            control::Frame report{control::Kind::Request, detail::lifecycle, detail::report_failure,
                                  detail::name_registration_request + 1};
            control::put_u32(report.payload, detail::primary_failure);
            const auto encoded = detail::encode_error(error);
            report.payload.insert(report.payload.end(), encoded.begin(), encoded.end());
            if (session->send(std::move(report))) {
                (void)session->receive_until(control::Clock::now() + config::module_error_report_timeout);
            }
            return child_exit::application_exception;
        }

        auto logs = log::detail::Host::connect(**service, args[detail::bootstrap::endpoint], options);
        detail::ModuleLocks locks(session, bootstrap->generation);
        detail::ModuleParams params(session, locks);
        RuntimeInbox inbox;
        inbox.logs = logs.get();
        inbox.params = &params;
        inbox.locks = &locks;
        inbox.stop_grace = std::chrono::milliseconds(bootstrap->grace);
        RuntimeGuard guard{inbox, session, **client, {}};
        try {
            guard.watchdog = std::jthread([&](std::stop_token token) {
                watch_central(session, **client, inbox, std::chrono::milliseconds(bootstrap->grace), token);
            });
            auto result = [&]() -> Result<void> {
                if (bootstrap->debug) {
                    auto released = wait_debug_gate(session, inbox);
                    if (!released) {
                        return std::unexpected(released.error());
                    }
                    if (!*released) {
                        return {};
                    }
                }
                auto path = osal::path_from_utf8(args[detail::bootstrap::plugin_path]);
                if (!path) {
                    return std::unexpected(wrap_error(ErrorCode::PluginLoadFailed,
                                                      ErrorOperation::LoadPlugin, path.error()));
                }
                auto loaded = detail::LoadedPlugin::load(*path, args[detail::bootstrap::module_type],
                                                         runtime_services.services());
                if (!loaded) {
                    return std::unexpected(loaded.error());
                }
                plugin.emplace(std::move(*loaded));
                return run_hooks(args, *bootstrap, session, *client, inbox, *plugin);
            }();
            if (!result) {
                // Already reported failures are not duplicated; early factory/name failures use this path.
                bool needs_report = false;
                {
                    std::lock_guard lock(inbox.mutex);
                    needs_report = inbox.next_fault_request == detail::name_registration_request + 1;
                }
                if (needs_report) {
                    (void)report_failure(session, inbox, result.error(), detail::primary_failure);
                }
                return child_exit::application_exception;
            }
            return 0;
        } catch (const std::system_error& error) {
            if (guard.watchdog.joinable()) {
                (void)report_failure(session, inbox,
                                     Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise,
                                           osal::native_error(error.code())},
                                     detail::primary_failure);
            } else {
                return exit_with_error(child_exit::application_exception,
                                       Error{ErrorCode::ThreadFailed, ErrorOperation::StartWorker,
                                             osal::native_error(error.code())});
            }
        } catch (const std::bad_alloc&) {
            // Reporting allocates; exhaustion cannot guarantee transport diagnostics.
            return child_exit::application_exception;
        } catch (...) {
            (void)report_failure(session, inbox,
                                 Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise},
                                 detail::primary_failure);
        }
    } catch (...) {
        // OS entry boundary: no exception escapes main. Central still observes the actual OS exit.
    }
    return child_exit::application_exception;
}
} // namespace vinc
