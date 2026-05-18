#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>

#include <module/module_manager.hpp>

#include <stem_shell/stem_shell.hpp>


namespace holon
{

StemShell::StemShell()
: _running(true)
{
    init_basic_functions();
}

// 將指令字串拆解為 vector<string>
std::vector<std::string> StemShell::tokenize(const std::string& line) 
{
    std::stringstream ss(line);
    std::string token;
    std::vector<std::string> tokens;

    while (ss >> token)
    {
        tokens.push_back(token);
    }
    
    return tokens;
}

void StemShell::process_line(const std::string& line) 
{
    //g_sigint_count = 0;

    auto args = tokenize(line);
    if (args.empty()) return;

    std::string target = args[0];

    int32_t ret = _module_manager.call_micro_program(target, args);

    if (ret == holon::SUCCESS) 
    {
        return; 
    } 
    else if (ret > -100 && ret < 0) 
    {
        // 微程式存在，但執行時回報錯誤 (-1 ~ -99)
        std::cerr << "\033[31m[" << target << "]\033[0m '" << target 
                  << "' failed: " << error_to_string(ret) << std::endl;
        return;
    }

    // 2. 檢查是否為已經在跑的 Node
    if (_module_manager.has_active_node(target)) 
    {
        int32_t ret = _module_manager.call_node_interface(target, args);
        if (ret != 0) std::cerr << "[Error] Node '" << target << "' return: " << ret << std::endl;
        return;
    }

    // 3. 檢查是否為可以啟動的 Module Factory
    if (_module_manager.has_factory(target)) 
    {
        int32_t ret = _module_manager.call_module_interface(target, args);
        if (ret != 0) std::cerr << "[Error] Module '" << target << "' return: " << ret << std::endl;
        return;
    }

    std::cout << "Unknown command: " << target << std::endl;

    microHelp(args);
}

void StemShell::handle_tab_completion(std::string& buffer) 
{
    if (buffer.empty()) return;

    auto args = tokenize(buffer);

    bool ends_with_space = (buffer.back() == ' ');
    std::vector<std::string> matches;
    
if (args.size() <= 1 && !ends_with_space) 
    {
        std::string prefix = args.empty() ? "" : args[0];

        // 1. 搜尋微程式 (例如 exit, help)
        for (auto const& name : _module_manager.get_micro_program_names()) {
            if (name.find(prefix) == 0) matches.push_back(name);
        }

        // 2. 搜尋 Registered Factories (模組藍圖)
        for (auto const& f : _module_manager.get_registered_factory_names()) {
            if (f.find(prefix) == 0) matches.push_back(f);
        }

        // 3. 搜尋 Active Nodes (執行中的神經元)
        for (auto const& n : _module_manager.get_active_node_names()) {
            if (n.find(prefix) == 0) matches.push_back(n);
        }

        // 只做排序，確保輸出整齊
        std::sort(matches.begin(), matches.end());
    } 
    // --- 場景 B: 補完參數 (Arguments) ---
    else 
    {
        // ... (這部分邏輯維持不變，呼叫模組或節點的 autoComplete) ...
        std::string target = args[0];
        std::string prefix = ends_with_space ? "" : args.back();
        std::vector<std::string> sub_options;

        if (_module_manager.has_active_node(target)) {
            sub_options = _module_manager.call_node_autoComplete(target, args);
        } else if (_module_manager.has_factory(target)) {
            sub_options = _module_manager.call_module_autoComplete(target, args);
        }

        for (const auto& opt : sub_options) {
            if (opt.find(prefix) == 0) matches.push_back(opt);
        }
        std::sort(matches.begin(), matches.end());
    }

    // --- 顯示結果 ---
    if (matches.size() == 1) {
        std::string current_token = (ends_with_space || args.empty()) ? "" : args.back();
        std::string completion = matches[0].substr(current_token.length());
        buffer += completion;
        std::cout << completion << std::flush;
    } 
    else if (matches.size() > 1) {
        std::cout << "\n";
        for (const auto& m : matches) {
            // 這裡不刪除，而是加上標籤。如果有兩個一樣的名字，你會看到兩個標籤
            if (_module_manager.has_micro_program(m)) std::cout << "[Cmd]" << m << "  ";
            if (_module_manager.has_active_node(m))  std::cout << "\033[36m[Node]\033[0m" << m << "  ";
            if (_module_manager.has_factory(m))     std::cout << "\033[33m[Mod]\033[0m" << m << "  ";
        }
        std::cout << "\n" << _prompt << buffer << std::flush;
    }
}

int8_t StemShell::init_basic_functions()
{
    _module_manager.register_micro_program(
        "exit", 
        [this](auto& ) 
        {
            this->_running = false;
            return 0;
            }
        );

    _module_manager.register_micro_program(
        "help",
        [this](auto& args) { return this->microHelp(args); }
    );

    return 0;
}

int32_t StemShell::microHelp(std::vector<std::string>& )
{
    std::cout << "\n==== [holon System Status] ====" << std::endl;
    
    // 1. 列出 Shell 內建的微程式 (microPrograms)
    std::cout << "\n[Shell Commands]:" << std::endl;
    auto microPrograms = _module_manager.get_micro_program_names();
    if (microPrograms.empty())
    {
        std::cout << "  (None)" << std::endl;
    } 
    for (const auto& name : microPrograms) {
        std::cout << "  > " << name << std::endl;
    }

    // 2. 列出可用的模組 (藍圖)
    std::cout << "\n[Available Modules]:" << std::endl;
    auto factories = _module_manager.get_registered_factory_names();
    if (factories.empty()) std::cout << "  (None)" << std::endl;
    for (const auto& f : factories) std::cout << "  - " << f << std::endl;

    // 3. 列出正在執行的節點 (活著的神經元)
    std::cout << "\n[Active Nodes]:" << std::endl;
    auto nodes = _module_manager.get_active_node_names();
    if (nodes.empty()) std::cout << "  (None)" << std::endl;
    for (const auto& n : nodes) std::cout << "  * " << n << std::endl;
    
    std::cout << "\n==============================\n" << std::endl;

    return 0;
}

void StemShell::run()
{
    std::string buffer;

    std::cout << "HOLON System Initialized. Type 'exit' to quit.\n";
    std::cout << _prompt << std::flush;

    while(_running)
    {
        char c = _terminal_guard.get_char();

            if (c == '\n' || c == '\r') 
            {
                std::cout << std::endl;
                if (!buffer.empty()) {
                    process_line(buffer);
                    buffer.clear();
                }
                // 如果在執行完指令後 _running 變成 false，就不再印 prompt
                if (_running) std::cout << _prompt << std::flush;
            } 
            else if (c == '\t') 
            {
                handle_tab_completion(buffer);
            }
            else if (c == 127 || c == 8) 
            { // Backspace
                if (!buffer.empty()) {
                    buffer.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            }
            else if (c == 3)
            {
                ctrl_c_count++;
                if(ctrl_c_count >= 4)
                {
                    _running = false;
                }
                else
                {
                    std::cout << "\n[Interrupt] Press " << (4 - ctrl_c_count) 
                              << " more times to force exit." << std::endl;
                    std::cout << _prompt << buffer << std::flush; // 重新印出 prompt 與緩衝區內容
                }
            }
            else 
            {
                // 這裡可以過濾掉不想要的控制字元
                buffer += c;
                std::cout << c << std::flush;
            }
        }
    }

} // namespace holon

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    holon::StemShell shell;

    //TODO : run script

    shell.run();

    return 0;
}