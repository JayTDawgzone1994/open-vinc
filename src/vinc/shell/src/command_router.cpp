// Resolve builtins, ModuleType starts, Module commands and legacy micro-programs.
// Return a structured result so terminal display and Tcl errors share the same dispatch behavior.

#include <vinc/shell/detail/command_router.hpp>
#include <vinc/module/detail/manager_access.hpp>
#include <algorithm>
#include <sstream>
#include <charconv>
#include <vinc/osal/path.hpp>

namespace vinc::shell {
std::string command_text(const Result<CommandReply>& result) {
    if (!result) {
        return vinc::error_to_string(result.error());
    }

    return result->message;
}

namespace {
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

Result<CommandReply> CommandRouter::execute(const std::vector<std::string>& arguments, std::stop_token stop) {
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::Execute});
    }

    if (arguments.empty()) {
        return {};
    }

    const auto& target = arguments.front();
    if (const auto builtin_entry = builtins_.find(target); builtin_entry != builtins_.end()) {
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

    return make_help();
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

Result<CommandReply> CommandRouter::make_help() {
    std::ostringstream help;
    help << "[Commands] debug start <ModuleType> [args...], debug continue <MID>, debug stop <MID>\n"
            "log status|reopen|level MODULE_OR_MID_OR_all error|warn|info|debug\n"
            "log show on|off|error|warn|info|debug; log show module NAME_OR_MID_OR_all\n"
            "param list|get NAME|set NAME VALUE|save|status\nhelp, srun file.tcl [args...], scancel, sstatus, output on|off, exit\n[Remote] "
            "disconnect, resume <code>\nCtrl+D cancels a local script; Ctrl+C shuts down the "
            "framework.\n[ModuleTypes]";
    for (const auto& name : manager_.get_registered_module_type_names()) {
        help << "\n  " << name;
    }

    help << "\n[Modules]";
    for (const auto& snapshot : manager_.snapshots()) {
        help << "\n  " << (snapshot.name.empty() ? "<unnamed>" : snapshot.name) << " "
             << state_name(snapshot.state) << " MID=" << snapshot.mid << " PID=" << snapshot.pid;
        if (snapshot.debug) {
            help << " DEBUG";
            if (snapshot.debug_waiting) {
                help << " (waiting before Plugin load)";
            }
        }
        if (snapshot.last_failure) {
            help << " : MID=" << snapshot.last_failure->mid << " "
                 << vinc::error_to_string(snapshot.last_failure->error);
        }

        if (snapshot.last_contract_failure) {
            help << "\n    " << contract_diagnostic_text(snapshot.name, *snapshot.last_contract_failure);
        }
        if (snapshot.cleanup_failure) {
            help << " cleanup MID=" << snapshot.cleanup_failure->mid << " "
                 << vinc::error_to_string(snapshot.cleanup_failure->error);
        }
    }

    auto legacy = manager_.get_micro_program_names();
    if (!legacy.empty()) {
        help << "\n[Registered commands]";
        for (const auto& name : legacy) {
            help << "\n  " << name;
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

    for (const auto& names : {manager_.get_registered_module_type_names(), manager_.get_active_module_names(),
                              manager_.get_micro_program_names()}) {
        result.choices.insert(result.choices.end(), names.begin(), names.end());
    }
}

Result<void> CommandRouter::collect_argument_choices(CompletionResult& result,
                                                     const std::vector<std::string>& arguments) {
    const auto target = arguments.front();
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
