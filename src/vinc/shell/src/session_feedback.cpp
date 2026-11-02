#include <vinc/shell/detail/session_feedback.hpp>
#include <vinc/param/error.hpp>
#include <vinc/log/error.hpp>

namespace vinc::shell {
namespace {
// Keep complete UTF-8 code points and prevent diagnostic text from issuing terminal controls.
// Storage reuses the existing command-line and Tcl diagnostic byte budgets.
std::string display_text(std::string_view text, std::size_t limit, bool multiline = false) {
    constexpr std::string_view marker = " [truncated]";
    auto count = text.size();
    const bool truncated = count > limit;
    if (truncated) {
        count = limit - marker.size();
        while (count && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80) {
            --count;
        }
    }

    std::string result(text.substr(0, count));
    for (auto& byte : result) {
        const auto value = static_cast<unsigned char>(byte);
        if ((value < 32 && !(multiline && (byte == '\n' || byte == '\t'))) || value == 127) {
            byte = '?';
        }
    }
    if (truncated) {
        result += marker;
    }
    return result;
}

std::string_view uncertain_outcome(const Error& error) {
    if (error.is(param::ErrorCode::TimeoutOutcomeUnknown) ||
        error.is(module_system::ErrorCode::TimeoutOutcomeUnknown)) {
        return "Timed out; execution outcome is unknown. Check state before retrying.";
    }
    if (error.is(param::ErrorCode::CancelledOutcomeUnknown) ||
        error.is(module_system::ErrorCode::CancelledOutcomeUnknown)) {
        return "Wait cancelled; execution outcome is unknown. Check state before retrying.";
    }
    if (error.is(module_system::ErrorCode::SessionEndedOutcomeUnknown) ||
        error.is(module_system::ErrorCode::GenerationChangedOutcomeUnknown)) {
        return "Connection or Module changed; execution outcome is unknown. Check state before retrying.";
    }
    return {};
}

std::string_view known_reason(const Error& error) {
    if (error.is(module_system::ErrorCode::SystemModuleProtected)) {
        return "System Module lifecycle is managed by the framework. Use 'ui' or 'recorder' commands.";
    }
    if (error.is(param::ErrorCode::NotFound)) {
        return "Parameter not found.";
    }
    if (error.is(param::ErrorCode::InvalidValue)) {
        return "Invalid parameter value or syntax. Type 'param --help'.";
    }
    if (error.is(param::ErrorCode::TypeMismatch)) {
        return "Parameter type mismatch.";
    }
    if (error.is(param::ErrorCode::PermissionDenied)) {
        return "Parameter access denied.";
    }
    if (error.is(log::ErrorCode::InvalidArgument)) {
        return "Invalid log arguments or target. Type 'log --help'.";
    }
    if (error.is(module_system::ErrorCode::NameExists)) {
        return "Module name already exists.";
    }
    if (error.is(module_system::ErrorCode::NameReserved)) {
        return "Module name is reserved by the framework.";
    }
    if (error.is(module_system::ErrorCode::NotRunning)) {
        return "Module is not running.";
    }
    if (error.is(module_system::ErrorCode::HookFailed)) {
        return "Module hook failed.";
    }
    if (error.is(module_system::ErrorCode::ProcessExited)) {
        return "Module process exited.";
    }
    return {};
}

std::string summary(const Error& error, std::string_view target) {
    if (error.is(ErrorCode::UnknownCommand)) {
        return "Unknown command: " + std::string(target) + ". Type 'help' for available commands.";
    }
    if (error.is(ErrorCode::InvalidArgument)) {
        return "Invalid arguments. Type '" + std::string(target.empty() ? "help" : target) + " --help'.";
    }
    if (error.is(ErrorCode::UnterminatedQuote)) {
        return "Unclosed quote. Close it before submitting the command.";
    }
    if (error.is(ErrorCode::LineTooLong)) {
        return "Rejected oversized command (maximum " + std::to_string(config::shell_max_line_bytes) + " bytes).";
    }
    if (error.is(ErrorCode::TooManyArguments)) {
        return "Too many command arguments (maximum " + std::to_string(config::shell_max_arguments) + ").";
    }
    if (error.is(ErrorCode::InvalidCharacter)) {
        return "Rejected invalid UTF-8 or control character.";
    }
    if (error.is(ErrorCode::Busy)) {
        return "Busy: another operation is still in progress.";
    }
    if (error.is(ErrorCode::Closing)) {
        return "Framework is shutting down.";
    }
    if (error.is(ErrorCode::SessionRequired)) {
        return "This command requires an interactive Shell session.";
    }
    if (error.is(ErrorCode::ResumeRejected)) {
        return "Resume rejected: code unavailable or expired.";
    }
    if (error.is(ErrorCode::TimeoutOutcomeUnknown)) {
        return "Timed out; execution outcome is unknown. Check state before retrying.";
    }
    if (error.is(ErrorCode::CancelledOutcomeUnknown)) {
        return "Wait cancelled; execution outcome is unknown. Check state before retrying.";
    }
    if (error.is(ErrorCode::OutcomeUnknown)) {
        return "Connection or Module changed; execution outcome is unknown. Check state before retrying.";
    }
    if (error.is(ErrorCode::Timeout)) {
        return "Timed out waiting for the operation.";
    }
    if (error.is(ErrorCode::Cancelled)) {
        return "Operation cancelled.";
    }

    // Never hide uncertain execution behind a generic wrapper such as HookFailed.
    auto reason = uncertain_outcome(error);
    for (const auto& cause : error.causes()) {
        if (!reason.empty()) {
            break;
        }
        reason = uncertain_outcome(Error::from_raw(cause.domain, cause.code, cause.operation, cause.native));
    }
    if (reason.empty()) {
        reason = known_reason(error);
    }
    if (reason.empty()) {
        // Causes explain the display only; the original outer error remains the API result.
        for (const auto& cause : error.causes()) {
            reason = known_reason(Error::from_raw(cause.domain, cause.code, cause.operation, cause.native));
            if (!reason.empty()) {
                break;
            }
        }
    }
    if (reason.empty()) {
        if (error.is(ErrorCode::StartFailed)) {
            reason = "Unable to start Module.";
        } else if (error.is(ErrorCode::CompletionFailed)) {
            reason = "Unable to complete this command.";
        } else if (error.is(ErrorCode::WorkerFailed)) {
            reason = "Unable to start command worker.";
        } else if (error.is(ErrorCode::OutputUnavailable)) {
            reason = "Module output is unavailable.";
        } else {
            reason = "Operation failed.";
        }
    }
    return std::string(reason) + " Use 'shell -e' for details.";
}
}

std::string SessionFeedback::failure(const Error& error, std::string_view target) {
    const auto name = display_text(target, config::shell_max_line_bytes);
    auto message = summary(error, name);
    if (!name.empty() && !error.is(ErrorCode::UnknownCommand)) {
        message = name + ": " + message;
    }
    context_ = name;
    script_details_.clear();
    failure_ = error;
    recorded_ = true;
    return message;
}

std::string SessionFeedback::command(const Result<CommandReply>& result, std::string_view target) {
    if (!result) {
        return failure(result.error(), target);
    }
    if (result->code != 0) {
        // Application status is not converted into a framework transport error.
        context_ = display_text(target, config::shell_max_line_bytes) + " returned " + std::to_string(result->code);
        failure_.reset();
        script_details_.clear();
        recorded_ = true;
        return context_;
    }
    return result->message;
}

std::string SessionFeedback::script(const ScriptReport& report) {
    if (report.outcome) {
        return script_text(report);
    }
    const auto location = "Script " + display_text(report.diagnostic.file, config::shell_max_line_bytes) +
                          " (line " + std::to_string(report.diagnostic.line) + ")";
    auto message = failure(report.outcome.error());
    context_ = location;
    script_details_ = display_text(report.diagnostic.details, config::shell_script_result_bytes, true);
    if (report.outcome.error().is(ErrorCode::ScriptFailed) && !script_details_.empty()) {
        const auto first_line = std::string_view(script_details_).substr(0, script_details_.find('\n'));
        message = display_text(first_line, config::shell_max_line_bytes) + " Use 'shell -e' for details.";
    }
    if (report.outcome.error().is(ErrorCode::Cancelled)) {
        return "Script canceled: " + display_text(report.diagnostic.file, config::shell_max_line_bytes);
    }
    return location + ": " + message;
}

std::string SessionFeedback::details() const {
    if (!recorded_) {
        return "No recorded error.";
    }
    std::string result = context_;
    if (failure_) {
        if (!result.empty()) {
            result += '\n';
        }
        result += error_to_string(*failure_);
    }
    if (!script_details_.empty()) {
        result += '\n';
        result += script_details_;
    }
    // Leave room for the Socket reply's prompt without exceeding its payload budget.
    constexpr auto limit = config::shell_socket_payload_bytes - config::shell_script_result_suffix_reserve;
    static_assert(limit > config::shell_max_line_bytes);
    return display_text(result, limit, true);
}
}
