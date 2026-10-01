#pragma once
#include <module/module_manager.hpp>
#include <stem_shell/command_line.hpp>
#include <map>

namespace vinc::shell {
struct CommandResult { bool success = true; bool shutdown = false; std::string message; };
struct CompletionResult { std::string input; std::size_t replace_start = 0; std::vector<std::string> choices; std::string error; };
std::string describe(const control::Error&);
// Shell owns its builtins. No callback capturing the Shell is stored in ModuleManager.
class CommandRouter {
    enum class Builtin { Help, Exit };
    ModuleManager& manager_;
    const std::map<std::string, Builtin> builtins_{{"exit", Builtin::Exit}, {"help", Builtin::Help}};
public:
    explicit CommandRouter(ModuleManager&);
    bool builtin(std::string_view) const;
    CommandResult execute(const std::vector<std::string>&);
    CompletionResult complete(std::string input);
};
}
