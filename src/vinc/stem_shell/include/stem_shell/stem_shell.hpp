#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <atomic>
#include <csignal>
#include <map>
#include <functional>
#include <filesystem>

#include <module/module_manager.hpp>

#include <stem_shell/terinal_guard.hpp>
#include <stem_shell/scriptInterpreter.hpp>

namespace vinc
{

class StemShell
{

public:
    StemShell();
    ~StemShell() = default;

    void run();

    int8_t process_line(const std::string& line);

    int8_t run_script(std::filesystem::path file_path);

    const std::string get_last_error() const { return _last_error_str; }

private:

    //TODO :void TCP_UDP_SHELL();
    ScriptInterpreter _script_terp;

    int8_t init_basic_functions();

    void handle_tab_completion(std::string& buffer);

    void handle_history_scroll(bool is_up, std::string& buffer);
    
    std::vector<std::string> tokenize(const std::string& line);

    int32_t microHelp(std::vector<std::string>& );

    TerminalGuard _terminal_guard;

    ModuleManager& _module_manager = vinc::ModuleManager::get();

    std::atomic<bool> _running;

    uint8_t ctrl_c_count = 0;

    std::string _last_error_str;

    std::string _prompt = "\033[32mVINC\033[0m > ";

    std::vector<std::string> _history; // 儲存歷史指令的口袋
    int32_t _history_index = -1;       // 目前循環到哪一條指令（-1 代表沒有在循環）

};

} // namespace vinc