#pragma once

// Shared command dispatch for interactive Shell and Tcl.
// The router resolves names and waits for results; callers choose how to schedule that wait.

#include <vinc/module/host/module_manager.hpp>
#include <vinc/shell/detail/command_line.hpp>
#include <map>

namespace vinc::shell {
struct CommandReply {
    std::int32_t code = 0;
    bool shutdown = false;
    std::string message;
};

struct CompletionResult {
    std::string input;
    std::size_t replace_start = 0;
    std::vector<std::string> choices;
};

// Rendering is a presentation boundary; dispatch retains numeric errors.
std::string command_text(const Result<CommandReply>& result);
std::string contract_diagnostic_text(std::string_view name, const ModuleContractDiagnostic& diagnostic);

// Shell owns its builtins. No callback capturing the Shell is stored in ModuleManager.
class CommandRouter {
    enum class Builtin {
        Debug,
        Help,
        Exit,
        Script,
        Cancel,
        Session
    };
    ModuleManager& manager_;
    const std::map<std::string, Builtin> builtins_{
        {"debug", Builtin::Debug},
        {"exit", Builtin::Exit},      {"help", Builtin::Help},      {"srun", Builtin::Script},
        {"scancel", Builtin::Cancel}, {"output", Builtin::Session}, {"disconnect", Builtin::Session},
        {"resume", Builtin::Session}, {"sstatus", Builtin::Session}};

    Result<CommandReply> execute_builtin(Builtin builtin_kind, const std::vector<std::string>& arguments);
    Result<CommandReply> execute_debug(const std::vector<std::string>& arguments, std::stop_token stop);
    Result<CommandReply> make_help();
    Result<CommandReply> execute_legacy(const std::vector<std::string>& arguments);
    Result<CommandReply> execute_module(const std::vector<std::string>& arguments, std::stop_token stop);
    Result<CommandReply> start_module(const std::vector<std::string>& arguments, std::stop_token stop);
    void collect_target_choices(CompletionResult& result);
    Result<void> collect_argument_choices(CompletionResult& result,
                                          const std::vector<std::string>& arguments);

public:
    explicit CommandRouter(ModuleManager& manager) : manager_(manager) {
    }

    Result<void> initialize();
    bool builtin(std::string_view name) const;
    std::string contract_notifications(std::uint64_t& cursor);
    Result<CommandReply> execute(const std::vector<std::string>& arguments, std::stop_token stop = {});
    Result<CompletionResult> complete(std::string input);
};
}
