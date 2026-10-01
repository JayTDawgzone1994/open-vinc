// Resolve builtins, factory starts, instance commands and legacy micro-programs.
// Return a structured result so terminal display and Tcl errors share the same dispatch behavior.

#include <stem_shell/command_router.hpp>
#include <algorithm>
#include <sstream>

namespace vinc::shell {
using namespace std::chrono_literals;
std::string describe(const control::Error& e) {
    std::string category;
    switch (e.code) {
    case control::ErrorCode::Timeout: category = "Timeout"; break;
    case control::ErrorCode::Closed: category = "Closed"; break;
    case control::ErrorCode::Protocol: category = "Protocol"; break;
    case control::ErrorCode::Io: category = "IO"; break;
    case control::ErrorCode::Backpressure: category = "Busy"; break;
    case control::ErrorCode::InvalidArgument: category = "InvalidArgument"; break;
    }
    auto text = category + ": " + e.message;
    if (e.os_error) text += " (OS " + std::to_string(e.os_error->native.value()) + ": " + e.os_error->native.message() + ")";
    return text;
}
namespace {
const char* state_name(ModuleState state) {
    switch (state) {
    case ModuleState::Queued: return "Queued";
    case ModuleState::Starting: return "Starting";
    case ModuleState::Running: return "Running";
    case ModuleState::Stopping: return "Stopping";
    case ModuleState::RestartWaiting: return "RestartWaiting";
    case ModuleState::Stopped: return "Stopped";
    case ModuleState::Failed: return "Failed";
    }
    return "Unknown";
}
}
CommandRouter::CommandRouter(ModuleManager& manager) : manager_(manager) {
    for (const auto& [name, _] : builtins_) {
        auto result = manager_.reserve_command_name(name);
        if (!result) throw std::runtime_error(describe(result.error()));
    }
}
bool CommandRouter::builtin(std::string_view name) const { return builtins_.contains(std::string(name)); }
CommandResult CommandRouter::execute(const std::vector<std::string>& args, std::stop_token stop) {
    if (stop.stop_requested()) return {false, false, "Script canceled"};
    if (args.empty()) return {};
    const auto& target = args.front();
    if (const auto b = builtins_.find(target); b != builtins_.end()) {
        if (b->second == Builtin::Script || b->second == Builtin::Cancel)
            return {false, false, target + " requires a Shell script session"};
        if (args.size() != 1) return {false, false, target + " takes no arguments"};
        if (b->second == Builtin::Exit) return {true, true, {}};
        std::ostringstream out;
        out << "[Commands] help, srun file.tcl [args...], scancel, exit\nCtrl+D cancels a script; Ctrl+C shuts down the framework.\n[Modules]";
        for (const auto& name : manager_.get_registered_factory_names()) out << "\n  " << name;
        out << "\n[Instances]";
        for (const auto& s : manager_.snapshots()) {
            out << "\n  " << s.name << " " << state_name(s.state) << " pid=" << s.pid;
            if (!s.last_error.empty()) out << " : " << s.last_error;
        }
        auto legacy = manager_.get_micro_program_names();
        if (!legacy.empty()) { out << "\n[Registered commands]"; for (const auto& name : legacy) out << "\n  " << name; }
        return {true, false, out.str()};
    }
    if (manager_.has_micro_program(target)) {
        const auto code = manager_.call_micro_program(target, args);
        return {code == 0, false, code ? target + " returned " + std::to_string(code) : ""};
    }
    if (manager_.inspect(target)) {
        auto result = manager_.command(target, args, control::Clock::now() + 5s, stop);
        if (!result) return {false, false, target + ": " + describe(result.error())};
        return {*result == 0, false, *result ? target + " returned " + std::to_string(*result) : ""};
    }
    if (manager_.has_factory(target)) {
        auto started = manager_.start_module(target, args);
        if (!started) return {false, false, target + ": " + describe(started.error())};
        auto ready = manager_.wait_started(*started, stop);
        if (!ready) return {false, false, *started + ": " + describe(ready.error())};
        return {true, false, "Started " + *started};
    }
    return {false, false, "Unknown command: " + target};
}
CompletionResult CommandRouter::complete(std::string input) {
    CompletionResult result; result.input = std::move(input);
    auto parsed = parse_line(result.input, true);
    if (!parsed) { result.error = parsed.error(); return result; }
    result.replace_start = parsed->last_start;
    auto& args = parsed->arguments;
    const bool target_position = args.empty() || (args.size() == 1 && !parsed->trailing_space);
    const auto prefix = args.empty() || parsed->trailing_space ? "" : args.back();
    if (target_position) {
        for (const auto& [name, _] : builtins_) result.choices.push_back(name);
        for (const auto& names : {manager_.get_registered_factory_names(), manager_.get_active_node_names(), manager_.get_micro_program_names()})
            result.choices.insert(result.choices.end(), names.begin(), names.end());
    } else {
        const auto target = args.front();
        if (manager_.inspect(target)) {
            auto response = manager_.complete(target, args, control::Clock::now() + 1s);
            if (!response) result.error = describe(response.error());
            else result.choices = std::move(*response);
        } else if (manager_.has_micro_program(target)) result.choices = manager_.call_micro_program_autoComplete(target, args);
        else if (manager_.has_factory(target)) result.choices = manager_.call_module_autoComplete(target, args);
    }
    std::erase_if(result.choices, [&](const auto& value) { return !value.starts_with(prefix) || value.find_first_of("\r\n\x1b") != std::string::npos; });
    std::sort(result.choices.begin(), result.choices.end());
    result.choices.erase(std::unique(result.choices.begin(), result.choices.end()), result.choices.end());
    return result;
}
}
