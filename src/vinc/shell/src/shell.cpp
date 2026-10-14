#include <vinc/module/detail/manager_access.hpp>
#include <vinc/log/detail/manager.hpp>
#include <vinc/osal/path.hpp>
// Shell event loop: keep keyboard/control input responsive while command jobs run.
// A foreground script suppresses ordinary input but keeps cancel and full shutdown available.

#include <vinc/time.hpp>
#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <vinc/osal/terminal.hpp>
#include <vinc/output_hub/text_renderer.hpp>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
#include <variant>

namespace vinc {
using namespace std::chrono_literals;
using shell::ErrorCode;
using shell::ErrorOperation;

namespace {
// Session 1 belongs to the local terminal; remote sessions have separate identities.
constexpr std::uint64_t local_script_session = 1;
// Reserve the prompt and cursor margin when displaying the editable UTF-8 tail.
constexpr std::size_t prompt_columns = 8;
}

struct Shell::Impl {
    ModuleManager& manager;
    std::ostream& output;
    output_hub::Subscription captured_output;
    output_hub::TextRenderer output_renderer;
    shell::CommandRouter router;
    shell::LineEditor editor;
    std::atomic<bool> closing{false};
    std::optional<Error> run_failure;
    std::uint64_t reported_remote_failures = 0;
    std::uint64_t contract_cursor = 0;
    std::uint64_t param_cursor = 0;
    std::uint64_t log_cursor = 0;
    log::View log_view;
    vinc::Deadline next_remote_diagnostic{};
    using Reply = std::variant<Result<shell::CommandReply>, Result<shell::CompletionResult>>;

    struct Job {
        std::string target;
        std::string input;
        std::future<Reply> reply;
    };

    std::vector<Job> jobs;
    osal::Terminal* terminal = nullptr;
    bool prompt = false;
    bool shutdown_complete = false;
    bool rejected_line = false;
    vinc::Deadline next_report{};
    // Join the shared script service before destroying the Shell's command/output state.
    shell::ScriptService script;
    std::unique_ptr<shell::SocketShell> remote; // Join before destroying the shared script service.

    Impl(ModuleManager& module_manager, std::ostream& output_stream)
        : manager(module_manager), output(output_stream), router(module_manager), script(module_manager) {
    }

    Result<void> initialize() {
        auto reserved = router.initialize();
        if (!reserved) {
            return reserved;
        }

        auto script_ready = script.initialize();
        if (!script_ready) {
            return script_ready;
        }

        auto subscribed = subscribe_output();
        if (!subscribed) {
            return subscribed;
        }

        auto commands = reserve_script_commands();
        if (!commands) {
            return commands;
        }

        reserve_job_storage();
        return {};
    }

    // Acquired subscriptions remain member-owned and unwind if a later stage throws.
    Result<void> subscribe_output() {
        auto subscribed = manager.output_hub().subscribe();
        if (!subscribed) {
            return std::unexpected(wrap_error(ErrorCode::OutputUnavailable, ErrorOperation::SubscribeOutput,
                                              subscribed.error()));
        }

        captured_output = std::move(*subscribed);
        return {};
    }

    // Reservations are manager-owned and intentionally survive Shell recreation.
    Result<void> reserve_script_commands() {
        auto names = shell::tcl_reserved_commands();
        if (!names) {
            return std::unexpected(names.error());
        }

        for (const auto& name : *names) {
            auto result = manager.reserve_command_name(name);
            if (!result) {
                return std::unexpected(wrap_error(ErrorCode::ReservationFailed,
                                                  ErrorOperation::ReserveCommands, result.error()));
            }
        }

        return {};
    }

    void reserve_job_storage() {
        jobs.reserve(config::shell_max_jobs);
    }

    bool interactive() const {
        return terminal && terminal->interactive() && terminal->ansi_output();
    }

    void clear_prompt() {
        if (prompt && interactive()) {
            output << "\r\x1b[2K";
        }

        prompt = false;
    }

    void redraw() {
        if (closing.load() || script.active(local_script_session) || !interactive()) {
            return;
        }

        const auto columns = terminal->columns();
        output << "\r\x1b[2KVINC > "
               << editor.visible_tail(columns > prompt_columns ? columns - prompt_columns : 0) << std::flush;
        prompt = true;
    }

    void show(const std::string& message) {
        if (message.empty()) {
            return;
        }

        clear_prompt();
        output << message << '\n' << std::flush;
        redraw();
    }

    void shutdown() {
        if (closing.exchange(true)) {
            return;
        }

        script.shutdown();
        if (remote) {
            remote->begin_shutdown();
        }

        manager.begin_shutdown();
        clear_prompt();
        output << "Shutting down: waiting for all module processes...\n" << std::flush;
    }

    Result<shell::CommandReply> execute(const std::vector<std::string>& args, std::stop_token stop = {}) {
        try {
            return router.execute(args, stop);
        } catch (const std::exception&) {
            return std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Execute});
        } catch (...) {
            return std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Execute});
        }
    }

    // Bound concurrent Shell jobs and reject a second job for the same target.
    // ModuleManager separately guards requests already sent to the child process.
    Result<void> available(const std::string& target) {
        if (jobs.size() >= config::shell_max_jobs) {
            return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Execute});
        }

        for (const auto& job : jobs) {
            if (job.target == target) {
                return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Execute});
            }
        }

        return {};
    }

    void start_script(std::filesystem::path path, std::vector<std::string> args = {}) {
        start_script_source({std::move(path), std::nullopt}, std::move(args));
    }

    void start_script_source(shell::ScriptSource source, std::vector<std::string> args) {
        clear_prompt();
        auto started = script.start_source(local_script_session, std::move(source), std::move(args));
        show(started ? "Script running. scancel cancels the global script."
                     : vinc::error_to_string(started.error()));
    }

    void cancel_script() {
        show(script.cancel() ? "Script cancellation requested" : "No script is running");
    }

    void submit(std::string input, bool completion = false) {
        if (closing.load()) {
            return;
        }

        auto parsed = shell::parse_line(input, completion);
        if (!parsed) {
            show("Parse error: " + vinc::error_to_string(parsed.error()));
            return;
        }

        const auto target = parsed->arguments.empty() ? std::string{} : parsed->arguments.front();
        if (!completion && parsed->arguments.empty()) {
            return;
        }

        if (!completion && handle_local_command(parsed->arguments)) {
            return;
        }

        auto admitted = available(target);
        if (!admitted) {
            show(vinc::error_to_string(admitted.error()));
            return;
        }

        launch_job(target, std::move(input), std::move(parsed->arguments), completion);
    }

    // Session commands execute on the Shell thread, which owns prompt and output state.
    // Returning true means the command was handled, including any reported failure.
    bool handle_local_command(const std::vector<std::string>& arguments) {
        const auto& target = arguments.front();
        if (target == "sstatus") {
            if (arguments.size() != 1) {
                show("sstatus takes no arguments");
            } else if (script.active()) {
                show("Script running");
            } else {
                show("No script is running");
            }

            return true;
        }

        if (target == "log" && arguments.size() > 1 && arguments[1] == "show") {
            auto result = log::detail::view_command(log_view, arguments);
            show(result ? *result : error_to_string(result.error()));
            return true;
        }
        if (target == "output") {
            if (arguments.size() != 2 || (arguments[1] != "on" && arguments[1] != "off")) {
                show("Usage: output on|off");
            } else {
                captured_output.set_enabled(arguments[1] == "on");
                output_renderer = {};
                show("Module output " + arguments[1]);
            }

            return true;
        }

        if (target == "srun") {
            if (arguments.size() < 2) {
                show("Usage: srun file.tcl [args...]");
                return true;
            }

            auto path = osal::path_from_utf8(arguments[1]);
            if (!path) {
                show(error_to_string(wrap_error(shell::ErrorCode::InvalidArgument,
                                                shell::ErrorOperation::StartScript, path.error())));
                return true;
            }

            start_script(std::move(*path), {arguments.begin() + 2, arguments.end()});
            return true;
        }

        if (target == "scancel") {
            if (arguments.size() != 1) {
                show("scancel takes no arguments");
            } else {
                cancel_script();
            }

            return true;
        }

        // Debug start/stop may wait for a child; run them on the existing command worker.
        if (router.builtin(target) && target != "debug") {
            auto result = execute(arguments);
            if (result && result->shutdown) {
                shutdown();
            } else {
                show(shell::command_text(result));
            }

            return true;
        }

        return false;
    }

    Reply execute_job(const std::string& input, const std::vector<std::string>& arguments, bool completion) {
        if (!completion) {
            return execute(arguments);
        }

        try {
            return router.complete(input);
        } catch (const std::exception&) {
            return Result<shell::CompletionResult>{
                std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Complete})};
        } catch (...) {
            return Result<shell::CompletionResult>{
                std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::Complete})};
        }
    }

    // Jobs capture this until their futures are consumed (or joined by destruction).
    void launch_job(const std::string& target, std::string input, std::vector<std::string> arguments,
                    bool completion) {
        try {
            jobs.push_back({target, input,
                            std::async(std::launch::async,
                                       [this, input = std::move(input), args = std::move(arguments),
                                        completion]() -> Reply {
                                           return execute_job(input, args, completion);
                                       })});
        } catch (const std::system_error& failure) {
            show(vinc::error_to_string(Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker,
                                             osal::native_error(failure.code())}));
        } catch (const std::exception&) {
            show(vinc::error_to_string(Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker}));
        }
    }

    // Consume worker results only on the Shell thread, keeping editor state and output
    // out of command callbacks. Script errors are displayed without shutting down modules.
    void collect() {
        const auto log_text = detail::ManagerAccess::log_take(manager, log_view);
        if (!log_text.empty()) {
            show(log_text);
        }
        const auto log_diagnostic = detail::ManagerAccess::log_notification(manager, log_cursor);
        if (!log_diagnostic.empty()) {
            show(log_diagnostic);
        }
        const auto diagnostics = router.contract_notifications(contract_cursor);
        if (!diagnostics.empty()) {
            show(diagnostics);
        }
        const auto parameter_diagnostics = router.param_notifications(param_cursor);
        if (!parameter_diagnostics.empty()) {
            show(parameter_diagnostics);
        }
        collect_output();
        collect_script();
        collect_shutdown_requests();
        collect_jobs();
    }

    void collect_script() {
        auto delivery = script.take(local_script_session);
        const auto& script_output = delivery.output;
        if (!script_output.empty()) {
            clear_prompt();
            output << script_output << std::flush;
            redraw();
        }

        if (delivery.truncated) {
            show("[Script output truncated]");
        }

        if (delivery.result) {
            show(shell::script_text(*delivery.result));
            redraw();
        }
    }

    void collect_shutdown_requests() {
        if (remote) {
            const auto status = remote->status();
            const auto now = control::Clock::now();
            if (status.last_failure && status.failures != reported_remote_failures &&
                (now >= next_remote_diagnostic || !status.running)) {
                show("Socket Shell diagnostic (failures=" + std::to_string(status.failures) +
                     ", worker=" + (status.running ? "running" : "stopped") +
                     ", accepting=" + (status.accepting ? "yes" : "no") + "): " +
                     vinc::error_to_string(*status.last_failure));
                reported_remote_failures = status.failures;
                next_remote_diagnostic = now + config::shell_socket_diagnostic_interval;
            }
        }

        if (script.shutdown_requested() || (remote && remote->shutdown_requested())) {
            shutdown();
        }
    }

    void collect_jobs() {
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it->reply.wait_for(0ms) != std::future_status::ready) {
                ++it;
                continue;
            }

            const auto input = std::move(it->input);
            auto reply = it->reply.get();
            it = jobs.erase(it);
            if (const auto* command = std::get_if<Result<shell::CommandReply>>(&reply)) {
                if (*command && (*command)->shutdown) {
                    shutdown();
                } else {
                    show(shell::command_text(*command));
                }

                continue;
            }

            const auto& completion = std::get<Result<shell::CompletionResult>>(reply);
            if (closing.load() || editor.line() != input) {
                continue;
            }

            if (!completion) {
                show(vinc::error_to_string(completion.error()));
                continue;
            }

            apply_completion(*completion);
        }
    }

    void apply_completion(const shell::CompletionResult& completion) {
        // A late Tab response must not overwrite subsequently edited text.
        if (closing.load() || editor.line() != completion.input) {
            return;
        }

        if (completion.choices.size() == 1) {
            if (!editor.replace(editor.line().substr(0, completion.replace_start) +
                                shell::quote_argument(completion.choices.front()))) {
                show("Completion exceeds command length limit");
            }

            redraw();
        } else if (!completion.choices.empty()) {
            std::string text;
            for (const auto& choice : completion.choices) {
                text += shell::quote_argument(choice) + "  ";
            }

            show(text);
        }
    }

    // Only the Shell thread writes its ostream or touches prompt/editor state.
    // Bound each batch so continuous module output cannot starve keyboard input.
    void collect_output(bool finish = false) {
        auto text = output_renderer.render(captured_output.take(
            finish ? config::shell_final_output_batch_events : config::shell_output_batch_events));
        if (finish) {
            text += output_renderer.flush(control::Clock::now(), true);
        }

        if (!text.empty()) {
            clear_prompt();
            output << text << std::flush;
            redraw();
        }
    }

    void report_shutdown_wait(const std::string& reason) {
        output << "Still waiting:";
        for (const auto& snapshot : manager.snapshots()) {
            if (snapshot.state == ModuleState::Stopped || snapshot.state == ModuleState::Failed) {
                continue;
            }

            output << " " << (snapshot.name.empty() ? "<unnamed>" : snapshot.name) << "(MID=" << snapshot.mid
                   << ", PID=" << snapshot.pid << ")";
            if (snapshot.cleanup_failure) {
                output << " [cleanup MID=" << snapshot.cleanup_failure->mid << ": "
                       << vinc::error_to_string(snapshot.cleanup_failure->error) << "]";
            }
        }

        output << " " << reason << '\n' << std::flush;
    }

    // Shutdown is a wait for observed completion, not just a batch of terminate requests.
    // Keep collecting jobs while supervision and Message Bus cleanup finish; repeated Ctrl+C does not skip it.
    void wait_shutdown() {
        if (shutdown_complete) {
            return;
        }

        shutdown();
        for (;;) {
            const auto result =
                manager.wait_shutdown(control::Clock::now() + config::shell_shutdown_poll_interval);
            collect();
            if (result) {
                break;
            }

            const auto now = control::Clock::now();
            if (now >= next_report) {
                report_shutdown_wait(vinc::error_to_string(
                    wrap_error(ErrorCode::Closing, ErrorOperation::WaitShutdown, result.error())));
                next_report = now + config::shell_shutdown_report_interval;
            }
        }

        while (!jobs.empty() || script.active()) {
            collect();
            if (!jobs.empty() || script.active()) {
                std::this_thread::sleep_for(config::shell_job_poll_interval);
            }
        }

        collect_output(true);
        if (remote) {
            remote->finish_shutdown();
            // Report isolated network failures even if they occurred during final draining.
            collect_shutdown_requests();
        }

        const auto output_stats = manager.output_hub().stats();
        if (output_stats.truncated_streams || output_stats.failed_streams) {
            output << "Output summary: " << output_stats.truncated_streams << " streams not fully drained, "
                   << output_stats.failed_streams << " read failures.\n";
        }

        shutdown_complete = true;
        output << "All module processes exited; Message Bus cleanup complete.\n" << std::flush;
    }

    void submit_edited_line() {
        if (script.active(local_script_session)) {
            return;
        }

        if (rejected_line) {
            editor.replace({}); // Do not retain an executable truncated prefix in history.
        }

        auto line = editor.submit();
        clear_prompt();
        if (terminal->interactive()) {
            output << "VINC > " << line << '\n';
        }

        if (rejected_line) {
            rejected_line = false;
            show("Rejected oversized command");
        } else {
            submit(std::move(line));
        }

        redraw();
    }

    void handle_key(const osal::TerminalEvent& event, bool headless_deployment) {
        switch (event.key) {
        case osal::TerminalKey::Interrupt:
            shutdown();
            break;
        case osal::TerminalKey::Cancel:
            cancel_script();
            break;
        case osal::TerminalKey::End:
            if (headless_deployment) {
                std::this_thread::sleep_for(config::shell_headless_backoff);
            } else {
                shutdown();
            }

            break;
        case osal::TerminalKey::Text:
            if (script.active(local_script_session)) {
                break;
            }

            if (!editor.append(event.text) && !rejected_line) {
                rejected_line = true;
                show("Command exceeds " + std::to_string(config::shell_max_line_bytes) +
                     " bytes; this line will not execute");
            }

            redraw();
            break;
        case osal::TerminalKey::Backspace:
            if (!script.active(local_script_session)) {
                editor.backspace();
                redraw();
            }

            break;
        case osal::TerminalKey::Up:
            if (!script.active(local_script_session)) {
                editor.older();
                redraw();
            }

            break;
        case osal::TerminalKey::Down:
            if (!script.active(local_script_session)) {
                editor.newer();
                redraw();
            }

            break;
        case osal::TerminalKey::Tab:
            if (!script.active(local_script_session)) {
                submit(editor.line(), true);
            }

            break;
        case osal::TerminalKey::Enter:
            submit_edited_line();
            break;
        default:
            break;
        }
    }
};

Shell::Shell(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

Result<std::unique_ptr<Shell>> Shell::create(ModuleManager& manager, std::ostream& output) {
    try {
        auto impl = std::make_unique<Impl>(manager, output);
        auto initialized = impl->initialize();
        if (!initialized) {
            return std::unexpected(initialized.error());
        }

        return std::unique_ptr<Shell>(new Shell(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Create});
    }
}

Shell::~Shell() = default;

void Shell::request_shutdown() {
    impl_->shutdown();
}

void Shell::wait_shutdown() {
    impl_->wait_shutdown();
}

Result<osal::IpEndpoint> Shell::start_socket_shell(shell::SocketShellOptions options) {
    if (impl_->remote) {
        return std::unexpected(Error{ErrorCode::AlreadyStarted, ErrorOperation::StartSocket});
    }

    auto created = shell::SocketShell::create(impl_->manager, impl_->script, std::move(options));
    if (!created) {
        return std::unexpected(created.error());
    }

    impl_->remote = std::move(*created);
    return impl_->remote->endpoint();
}

Result<std::int32_t> Shell::process_line(const std::string& line) {
    if (impl_->closing.load()) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::Execute});
    }

    auto parsed = shell::parse_line(line);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto result = impl_->execute(parsed->arguments);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (result->shutdown) {
        impl_->shutdown();
    } else {
        impl_->show(result->message);
    }

    return result->code;
}

Result<void> Shell::run(std::filesystem::path startup_script, std::vector<std::string> script_args) {
    return run_source({std::move(startup_script), std::nullopt}, std::move(script_args));
}

Result<void> Shell::run_source(shell::ScriptSource startup_script, std::vector<std::string> script_args) {
    if (impl_->closing.load()) {
        impl_->wait_shutdown();
        if (impl_->run_failure) {
            return std::unexpected(*impl_->run_failure);
        }

        return {};
    }

    auto opened = osal::Terminal::open();
    if (!opened) {
        impl_->run_failure =
            wrap_error(ErrorCode::TerminalFailed, ErrorOperation::OpenTerminal, opened.error());
        impl_->wait_shutdown();
        return std::unexpected(*impl_->run_failure);
    }

    auto terminal = std::move(*opened);
    impl_->terminal = terminal.get();

    // Clear the borrowed pointer before the terminal owner restores OS input state.
    struct TerminalAttachmentGuard {
        Impl& state;

        ~TerminalAttachmentGuard() {
            state.terminal = nullptr;
        }
    };

    TerminalAttachmentGuard attachment{*impl_};

    // A startup deployment can intentionally have no terminal. Its stdin EOF must not
    // turn a finished or failed script into an automatic framework shutdown.
    const bool headless_deployment = (!startup_script.empty() || impl_->remote) && !terminal->interactive();
    if (!startup_script.empty()) {
        impl_->start_script_source(std::move(startup_script), std::move(script_args));
    }

    impl_->show("VINC ready. Type help; exit or Ctrl+C shuts down all modules.");
    impl_->redraw();
    try {
        while (!impl_->closing.load()) {
            impl_->collect();
            auto event = terminal->read_until(control::Clock::now() + config::shell_terminal_poll_interval);
            if (!event) {
                if (!impl_->run_failure) {
                    impl_->run_failure =
                        wrap_error(ErrorCode::TerminalFailed, ErrorOperation::ReadTerminal, event.error());
                }

                impl_->show(vinc::error_to_string(*impl_->run_failure));
                impl_->shutdown();
                break;
            }

            impl_->handle_key(*event, headless_deployment);
        }
    } catch (const std::exception&) {
        if (!impl_->run_failure) {
            impl_->run_failure = Error{ErrorCode::UnexpectedException, ErrorOperation::Run};
        }

        impl_->show(vinc::error_to_string(*impl_->run_failure));
    }

    impl_->wait_shutdown();
    if (impl_->run_failure) {
        return std::unexpected(*impl_->run_failure);
    }

    return {};
}
}
