#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <atomic>
#include <csignal>
#include <map>
#include <functional>

#include <stem_shell/terinal_guard.hpp>

namespace holon
{

class StemShell
{

public:
    StemShell();
    ~StemShell() = default;

    void run();

private:

    //TODO :void TCP_UDP_SHELL();

    int8_t init_basic_functions();

    void process_line(const std::string& line);

    void handle_tab_completion(std::string& buffer);
    
    std::vector<std::string> tokenize(const std::string& line);

    int32_t microHelp(std::vector<std::string>& );

    TerminalGuard _terminal_guard;

    ModuleManager& _module_manager = holon::ModuleManager::get();

    std::atomic<bool> _running;

    uint8_t ctrl_c_count = 0;

    std::string _prompt = "\033[32mHOLON\033[0m > ";

};

} // namespace holon