// Resolve builtins, ModuleType starts, Module commands and legacy micro-programs.
// Return a structured result so terminal display and Tcl errors share the same dispatch behavior.

#include <vinc/shell/detail/command_router.hpp>
#include <vinc/module/detail/manager_access.hpp>
#include <algorithm>
#include <array>
#include <sstream>
#include <charconv>
#include <vinc/osal/path.hpp>

namespace vinc::shell {
namespace {
// Fixed columns keep local and remote help identical without querying terminal width.
template<class Names>
void append_help_names(std::ostream& output, std::string_view heading, const Names& names) {
    constexpr std::size_t columns = 3;
    constexpr std::size_t column_gap = 2;
    std::array<std::size_t, columns> widths{};
    output << heading << '\n';
    if (names.empty()) {
        output << "  (none)\n";
        return;
    }

    for (std::size_t index = 0; index < names.size(); ++index) {
        auto& width = widths[index % columns];
        width = (std::max)(width, names[index].size());
    }

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto column = index % columns;
        if (column == 0) {
            output << "  ";
        }
        output << names[index];
        if (column + 1 == columns || index + 1 == names.size()) {
            output << '\n';
        } else {
            output << std::string(widths[column] - names[index].size() + column_gap, ' ');
        }
    }
}

// Expose retry-relevant state in the Shell domain. The original ModuleSystem
// operation and diagnostics remain in the cause chain; no automatic retry occurs.
Error module_failure(const Error& failure, ErrorCode fallback, ErrorOperation operation) {
    auto code = fallback;
    if (failure.is(module_system::ErrorCode::Busy)) {
        code = ErrorCode::Busy;
    } else if (failure.is(module_system::ErrorCode::Closing)) {
        code = ErrorCode::Closing;
    } else if (failure.is(module_system::ErrorCode::Timeout)) {
        code = ErrorCode::Timeout;
    } else if (failure.is(module_system::ErrorCode::Cancelled)) {
        code = ErrorCode::Cancelled;
    } else if (failure.is(module_system::ErrorCode::TimeoutOutcomeUnknown)) {
        code = ErrorCode::TimeoutOutcomeUnknown;
    } else if (failure.is(module_system::ErrorCode::CancelledOutcomeUnknown)) {
        code = ErrorCode::CancelledOutcomeUnknown;
    } else if (failure.is(module_system::ErrorCode::SessionEndedOutcomeUnknown) ||
               failure.is(module_system::ErrorCode::GenerationChangedOutcomeUnknown)) {
        code = ErrorCode::OutcomeUnknown;
    }

    return wrap_error(code, operation, failure);
}

const char* state_name(ModuleState state) {
    switch (state) {
    case ModuleState::Queued:
        return "Queued";
    case ModuleState::Starting:
        return "Starting";
    case ModuleState::Running:
        return "Running";
    case ModuleState::Stopping:
        return "Stopping";
    case ModuleState::RestartWaiting:
        return "RestartWaiting";
    case ModuleState::Stopped:
        return "Stopped";
    case ModuleState::Failed:
        return "Failed";
    }

    return "Unknown";
}
}

// Child text is untrusted terminal input. Keep UTF-8 bytes but remove C0/DEL
// controls (including ESC) before embedding it in framework diagnostics.
std::string contract_diagnostic_text(std::string_view name, const ModuleContractDiagnostic& diagnostic) {
    const auto safe = [](std::string_view text) {
        std::string result;
        for (const unsigned char byte : text) {
            result += byte < 32 || byte == 127 ? '?' : static_cast<char>(byte);
        }
        return result;
    };
    const auto text_view = [](const auto& text) {
        const auto end = std::find(text.begin(), text.end(), '\0');
        return std::string_view(text.data(), static_cast<std::size_t>(end - text.begin()));
    };
    std::ostringstream text;
    text << "[Module fault] " << safe(name.empty() ? "<unnamed>" : name)
         << " MID=" << diagnostic.mid << " PID=" << diagnostic.pid
         << " exited " << (diagnostic.exit.kind == osal::ExitKind::Code ? "code=" : "signal=")
         << diagnostic.exit.value
         << " OS termination requested=" << (diagnostic.termination_requested ? "yes" : "no")
         << " fatal exit deadline expired=" << (diagnostic.exit_deadline_expired ? "yes" : "no");
    if (!diagnostic.report) {
        text << " : " << error_to_string(diagnostic.report.error());
        return text.str();
    }

    const auto& report = *diagnostic.report;
    text << " : " << safe(text_view(report.reason));
    if (report.has_index) {
        text << " index=" << report.index << " size=" << report.size;
    }
    text << " at " << safe(text_view(report.file)) << ':' << report.line
         << " (" << safe(text_view(report.function)) << ')';
    if (report.text_truncated) {
        text << " [diagnostic text truncated]";
    }
    return text.str();
}

std::string CommandRouter::contract_notifications(std::uint64_t& cursor) {
    const auto revision = manager_.contract_diagnostic_revision();
    if (cursor == revision) {
        return {};
    }

    std::string text;
    for (const auto& snapshot : manager_.snapshots()) {
        if (!snapshot.last_contract_failure) {
            continue;
        }
        const auto& diagnostic = *snapshot.last_contract_failure;
        if (diagnostic.sequence > cursor && diagnostic.sequence <= revision) {
            if (!text.empty()) {
                text += '\n';
            }
            text += contract_diagnostic_text(snapshot.name, diagnostic);
        }
    }
    cursor = revision;
    return text;
}

std::string CommandRouter::param_notifications(std::uint64_t& cursor) {
    return detail::ManagerAccess::param_diagnostic(manager_, cursor);
}

Result<void> CommandRouter::initialize() {
    for (const auto& [name, _] : builtins_) {
        auto result = manager_.reserve_command_name(name);
        if (!result) {
            return std::unexpected(
                wrap_error(ErrorCode::ReservationFailed, ErrorOperation::ReserveCommands, result.error()));
        }
    }

    return {};
}

bool CommandRouter::builtin(std::string_view name) const {
    return builtins_.contains(std::string(name));
}

std::optional<std::string_view> CommandRouter::command_help(const std::vector<std::string>& arguments) const {
    if (arguments.size() != 2 || (arguments[1] != "-h" && arguments[1] != "--help")) {
        return std::nullopt;
    }

    // Static descriptions serve all command entry points without a runtime registry.
    struct HelpEntry {
        std::string_view name;
        std::string_view text;
    };
    static constexpr HelpEntry entries[]{
        {"ui", "Usage: ui enable | disable | status\nEnable/disable HTTP service; the supervised System Module stays alive."},
        {"recorder", "Usage: recorder record <DIR> (--all | --topic <NAME>...)\n"
                     "       [--database-bytes N] [--queue-bytes N] [--message-bytes N]\n"
                     "       recorder status | finish | info <DIR>\n"
                     "finish stops recording, not the System Module. A crash restarts idle."},
        {"shell", "Usage: shell -e | --error\nShow this session's last failure. Success does not clear it."},
        {"help", "Usage: help [--verbose]\nShow commands, user ModuleTypes and Modules.\n"
                 "--verbose also shows System Modules and retained fault diagnostics."},
        {"log", "Usage: log status\n       log reopen\n"
                "       log level <MODULE|MID|all> <error|warn|info|debug>\n"
                "       log show <on|off|error|warn|info|debug>\n"
                "       log show module <NAME|MID|all>\n"
                "show affects this interactive session only, not saved logs."},
        {"param", "Usage: param list\n       param get <NAME>\n       param set <NAME> <VALUE>\n"
                  "       param save\n       param status\nQuote values containing spaces.\n"
                  "Restart hints are declarations, not pending-state tracking; no automatic restart or save."},
        {"debug", "Usage: debug start <ModuleType> [args...]\n       debug continue <MID>\n"
                  "       debug stop <MID>\nAttach a debugger to the reported PID before continuing."},
        {"srun", "Usage: srun <file.tcl> [args...]\nRun a Tcl script; following arguments belong to the script."},
        {"scancel", "Usage: scancel\nRequest cancellation of the current script; native blocking work may delay it."},
        {"sstatus", "Usage: sstatus\nShow whether a script is running."},
        {"output", "Usage: output <on|off>\nShow or hide Module stdout/stderr in this session."},
        {"exit", "Usage: exit\nShut down the framework and wait for Module cleanup. Local shortcut: Ctrl+C."},
        {"disconnect", "Usage: disconnect\nDisconnect this Socket Shell session without shutting down the framework."},
        {"resume", "Usage: resume <code>\nRecover a disconnected Socket Shell session before its timeout."}
    };
    for (const auto& entry : entries) {
        if (arguments.front() == entry.name) {
            return entry.text;
        }
    }

    return std::nullopt;
}

Result<CommandReply> CommandRouter::execute(const std::vector<std::string>& arguments, std::stop_token stop) {
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::Execute});
    }

    if (arguments.empty()) {
        return {};
    }

    const auto& target = arguments.front();
    if (const auto builtin_entry = builtins_.find(target); builtin_entry != builtins_.end()) {
        if (const auto help = command_help(arguments)) {
            return CommandReply{0, false, std::string(*help)};
        }

        if (builtin_entry->second == Builtin::Log) {
            if (arguments.size() > 1 && arguments[1] == "show") {
                return std::unexpected(Error{ErrorCode::SessionRequired, ErrorOperation::Execute});
            }
            auto result = detail::ManagerAccess::log_command(
                manager_, std::vector<std::string>(arguments.begin() + 1, arguments.end()));
            if (!result) {
                return std::unexpected(
                    wrap_error(ErrorCode::CommandFailed, ErrorOperation::Execute, result.error()));
            }
            return CommandReply{0, false, std::move(*result)};
        }
        if (builtin_entry->second == Builtin::Param) {
            auto result = detail::ManagerAccess::param_command(
                manager_, std::vector<std::string>(arguments.begin() + 1, arguments.end()), stop);
            if (!result) {
                return std::unexpected(wrap_error(ErrorCode::CommandFailed, ErrorOperation::Execute, result.error()));
            }
            return CommandReply{0, false, std::move(*result)};
        }
        if (builtin_entry->second == Builtin::Debug) {
            return execute_debug(arguments, stop);
        }
        if (builtin_entry->second == Builtin::System) {
            return execute_system(arguments, stop);
        }
        return execute_builtin(builtin_entry->second, arguments);
    }

    if (manager_.has_micro_program(target)) {
        return execute_legacy(arguments);
    }

    if (manager_.inspect(target)) {
        return execute_module(arguments, stop);
    }

    if (manager_.has_module_type(target)) {
        return start_module(arguments, stop);
    }

    return std::unexpected(Error{ErrorCode::UnknownCommand, ErrorOperation::Execute});
}

Result<CommandReply> CommandRouter::execute_builtin(Builtin builtin_kind,
                                                    const std::vector<std::string>& arguments) {
    if (builtin_kind == Builtin::Help && arguments.size() == 2 && arguments[1] == "--verbose") {
        return make_help(true);
    }

    if (builtin_kind == Builtin::Script || builtin_kind == Builtin::Cancel ||
        builtin_kind == Builtin::Session) {
        return std::unexpected(Error{ErrorCode::SessionRequired, ErrorOperation::Execute});
    }

    if (arguments.size() != 1) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }

    if (builtin_kind == Builtin::Exit) {
        return CommandReply{0, true, {}};
    }

    return make_help(false);
}

Result<CommandReply> CommandRouter::execute_debug(const std::vector<std::string>& arguments,
                                                std::stop_token stop) {
    if (arguments.size() < 3) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }

    if (arguments[1] == "start") {
        // Preserve normal Module argv semantics, including the ModuleType at argv[0].
        std::vector<std::string> module_args(arguments.begin() + 2, arguments.end());
        auto started = manager_.start_module(arguments[2], std::move(module_args), {.debug = true});
        if (!started) {
            return std::unexpected(module_failure(started.error(), ErrorCode::StartFailed,
                                                  ErrorOperation::StartModule));
        }

        const auto deadline = control::Clock::now() + config::shell_command_timeout;
        auto ready = manager_.wait_debug_ready(*started, deadline, stop);
        if (!ready) {
            return std::unexpected(module_failure(ready.error(), ErrorCode::StartFailed,
                                                  ErrorOperation::StartModule));
        }

        const auto snapshot = manager_.inspect(*started);
        if (!snapshot) {
            return std::unexpected(Error{ErrorCode::StartFailed, ErrorOperation::StartModule});
        }

        auto path = osal::path_to_utf8(snapshot->plugin_path);
        if (!path) {
            return std::unexpected(wrap_error(ErrorCode::StartFailed, ErrorOperation::StartModule, path.error()));
        }

        // Paths are diagnostic text, not terminal escape sequences or executable commands.
        for (auto& byte : *path) {
            const auto value = static_cast<unsigned char>(byte);
            if (value < 32 || value == 127) {
                byte = '?';
            }

        }

        return CommandReply{0, false, "Debug waiting MID=" + std::to_string(*started) +
            " PID=" + std::to_string(snapshot->pid) + " Plugin=" + *path +
            "\nAttach debugger, set breakpoints, continue the debugger, then: debug continue " +
            std::to_string(*started)};
    }

    if (arguments.size() != 3 || (arguments[1] != "continue" && arguments[1] != "stop")) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }

    MID mid = 0;
    const auto& text = arguments[2];
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), mid);
    if (error != std::errc{} || end != text.data() + text.size() || mid == 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }

    auto result = arguments[1] == "continue"
                      ? manager_.continue_debug(mid)
                      : manager_.stop_module(mid, control::Clock::now() + config::shell_command_timeout);
    if (!result) {
        return std::unexpected(module_failure(result.error(), ErrorCode::CommandFailed, ErrorOperation::Execute));
    }

    return CommandReply{0, false, arguments[1] == "continue" ? "Debug startup release queued" : "Module stopped"};
}

Result<CommandReply> CommandRouter::execute_system(const std::vector<std::string>& arguments,
                                                   std::stop_token stop) {
    const auto& name = arguments.front();
    if (arguments.size() < 2) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }
    const auto snapshot = manager_.inspect(name);
    if (!snapshot || snapshot->kind != ModuleKind::System) {
        return std::unexpected(wrap_error(ErrorCode::CommandFailed, ErrorOperation::Execute,
            Error{module_system::ErrorCode::ModuleNotFound, module_system::ErrorOperation::Command}));
    }
    if (name == "ui" && arguments.size() == 2 && (arguments[1] == "enable" || arguments[1] == "disable")) {
        const bool enabled = arguments[1] == "enable";
        auto changed = detail::ManagerAccess::set_system_enabled(manager_, name, enabled,
            control::Clock::now() + config::shell_command_timeout, stop);
        if (!changed) {
            return std::unexpected(module_failure(changed.error(), ErrorCode::CommandFailed, ErrorOperation::Execute));
        }
        return CommandReply{0, false, enabled ? "UI enable requested." : "UI disable requested."};
    }
    if (name == "ui" && (arguments.size() != 2 || arguments[1] != "status")) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
    }
    if (arguments.size() == 2 && arguments[1] == "status" && snapshot->state != ModuleState::Running) {
        std::string state = name + " " + state_name(snapshot->state);
        if (snapshot->system_enabled) {
            state += *snapshot->system_enabled ? " desired=enabled" : " desired=disabled";
        }
        if (snapshot->restart_rate_limited) {
            state += " (restart rate limited)";
        }
        state += "; details: help --verbose";
        return CommandReply{0, false, std::move(state)};
    }
    return execute_module(arguments, stop);
}

Result<CommandReply> CommandRouter::make_help(bool verbose) {
    static constexpr std::array<std::string_view, 12> commands{
        "help", "shell", "log", "param", "debug", "srun", "scancel", "sstatus", "output", "ui", "recorder", "exit"};
    static constexpr std::array<std::string_view, 2> remote_commands{"disconnect", "resume"};
    std::ostringstream help;
    help << "Usage: <command> -h / --help; last error: shell -e\n\n";
    append_help_names(help, "[Commands]", commands);
    if (remote_session_) {
        help << '\n';
        append_help_names(help, "[Remote]", remote_commands);
    }
    help << '\n';
    append_help_names(help, "[ModuleTypes]", manager_.get_registered_module_type_names(false));

    help << "\n[Modules]";
    for (const auto& snapshot : manager_.snapshots()) {
        if (snapshot.kind == ModuleKind::System) {
            continue;
        }
        help << "\n  " << (snapshot.name.empty() ? "<unnamed>" : snapshot.name) << " "
             << state_name(snapshot.state) << " MID=" << snapshot.mid << " PID=" << snapshot.pid;
        if (snapshot.debug) {
            help << " DEBUG";
            if (snapshot.debug_waiting) {
                help << " (waiting before Plugin load)";
            }
        }
        if (verbose && snapshot.last_failure) {
            help << " : MID=" << snapshot.last_failure->mid << " "
                 << vinc::error_to_string(snapshot.last_failure->error);
        }

        if (verbose && snapshot.last_contract_failure) {
            help << "\n    " << contract_diagnostic_text(snapshot.name, *snapshot.last_contract_failure);
        }
        if (verbose && snapshot.cleanup_failure) {
            help << " cleanup MID=" << snapshot.cleanup_failure->mid << " "
                 << vinc::error_to_string(snapshot.cleanup_failure->error);
        }
    }

    if (verbose) {
        help << "\n[System Modules]";
        for (const auto& snapshot : manager_.snapshots()) {
            if (snapshot.kind != ModuleKind::System) {
                continue;
            }
            help << "\n  " << snapshot.name << " " << state_name(snapshot.state)
                 << " MID=" << snapshot.mid << " PID=" << snapshot.pid;
            if (snapshot.system_enabled) {
                help << " desired=" << (*snapshot.system_enabled ? "enabled" : "disabled");
            }
            if (snapshot.next_restart) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *snapshot.next_restart - control::Clock::now()).count();
                help << " retry_in_ms=" << (std::max)(std::int64_t{0}, remaining)
                     << " rate_limited=" << snapshot.restart_rate_limited;
            }
            if (snapshot.last_failure) {
                help << "\n    MID=" << snapshot.last_failure->mid << " " << error_to_string(snapshot.last_failure->error);
            }
            if (snapshot.last_contract_failure) {
                help << "\n    " << contract_diagnostic_text(snapshot.name, *snapshot.last_contract_failure);
            }
            if (snapshot.cleanup_failure) {
                help << "\n    cleanup MID=" << snapshot.cleanup_failure->mid << " " << error_to_string(snapshot.cleanup_failure->error);
            }
            if (snapshot.restart_failure) {
                help << "\n    restart: " << error_to_string(*snapshot.restart_failure);
            }
        }
    }

    auto legacy = manager_.get_micro_program_names();
    if (!legacy.empty()) {
        help << "\n[Registered commands]";
        for (const auto& name : legacy) {
            help << "  " << name;
        }
    }

    return CommandReply{0, false, help.str()};
}

Result<CommandReply> CommandRouter::execute_legacy(const std::vector<std::string>& arguments) {
    const auto& target = arguments.front();
    const auto code = manager_.call_micro_program(target, arguments);
    if (!code) {
        return std::unexpected(
            module_failure(code.error(), ErrorCode::CommandFailed, ErrorOperation::Execute));
    }

    return CommandReply{*code, false, *code ? target + " returned " + std::to_string(*code) : ""};
}

Result<CommandReply> CommandRouter::execute_module(const std::vector<std::string>& arguments,
                                                   std::stop_token stop) {
    const auto& target = arguments.front();
    auto result =
        manager_.command(target, arguments, control::Clock::now() + config::shell_command_timeout, stop);
    if (!result) {
        return std::unexpected(
            module_failure(result.error(), ErrorCode::CommandFailed, ErrorOperation::Execute));
    }

    return CommandReply{*result, false, *result ? target + " returned " + std::to_string(*result) : ""};
}

Result<CommandReply> CommandRouter::start_module(const std::vector<std::string>& arguments,
                                                 std::stop_token stop) {
    const auto& target = arguments.front();
    auto started = manager_.start_module(target, arguments);
    if (!started) {
        return std::unexpected(
            module_failure(started.error(), ErrorCode::StartFailed, ErrorOperation::StartModule));
    }

    auto ready = manager_.wait_started(*started, stop);
    if (!ready) {
        return std::unexpected(
            module_failure(ready.error(), ErrorCode::StartFailed, ErrorOperation::StartModule));
    }

    auto snapshot = manager_.inspect(*started);
    if (!snapshot) {
        return std::unexpected(Error{ErrorCode::StartFailed, ErrorOperation::StartModule});
    }

    return CommandReply{0, false,
                        "Started " + snapshot->name + " (MID=" + std::to_string(snapshot->mid) + ")"};
}

void CommandRouter::collect_target_choices(CompletionResult& result) {
    for (const auto& [name, _] : builtins_) {
        result.choices.push_back(name);
    }

    for (const auto& names : {manager_.get_registered_module_type_names(false), manager_.get_active_module_names(false),
                              manager_.get_micro_program_names()}) {
        result.choices.insert(result.choices.end(), names.begin(), names.end());
    }
}

Result<void> CommandRouter::collect_argument_choices(CompletionResult& result,
                                                     const std::vector<std::string>& arguments) {
    const auto target = arguments.front();
    if (target == "ui") {
        result.choices = {"enable", "disable", "status"};
        return {};
    }
    if (target == "recorder") {
        result.choices = {"record", "status", "finish", "info"};
        return {};
    }
    if (manager_.inspect(target)) {
        auto response =
            manager_.complete(target, arguments, control::Clock::now() + config::shell_completion_timeout);
        if (!response) {
            return std::unexpected(
                module_failure(response.error(), ErrorCode::CompletionFailed, ErrorOperation::Complete));
        } else {
            result.choices = std::move(*response);
        }
    } else if (manager_.has_micro_program(target)) {
        auto choices = manager_.call_micro_program_autoComplete(target, arguments);
        if (!choices) {
            return std::unexpected(
                wrap_error(ErrorCode::CompletionFailed, ErrorOperation::Complete, choices.error()));
        } else {
            result.choices = std::move(*choices);
        }
    } else if (manager_.has_module_type(target)) {
        auto choices = manager_.call_module_type_autoComplete(target, arguments);
        if (!choices) {
            return std::unexpected(
                wrap_error(ErrorCode::CompletionFailed, ErrorOperation::Complete, choices.error()));
        } else {
            result.choices = std::move(*choices);
        }
    }

    return {};
}

Result<CompletionResult> CommandRouter::complete(std::string input) {
    CompletionResult result;
    result.input = std::move(input);
    auto parsed = parse_line(result.input, true);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    result.replace_start = parsed->last_start;
    auto& arguments = parsed->arguments;
    const bool target_position = arguments.empty() || (arguments.size() == 1 && !parsed->trailing_space);
    const auto prefix = arguments.empty() || parsed->trailing_space ? "" : arguments.back();
    if (target_position) {
        collect_target_choices(result);
    } else {
        auto collected = collect_argument_choices(result, arguments);
        if (!collected) {
            return std::unexpected(collected.error());
        }
    }

    std::erase_if(result.choices, [&](const auto& value) {
        return !value.starts_with(prefix) || value.find_first_of("\r\n\x1b") != std::string::npos;
    });
    std::sort(result.choices.begin(), result.choices.end());
    result.choices.erase(std::unique(result.choices.begin(), result.choices.end()), result.choices.end());
    return result;
}
}
