#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>

#include <module/module_manager.hpp>

#include <stem_shell/stem_shell.hpp>


namespace vinc
{

StemShell::StemShell()
:_script_terp(this), 
_running(true)
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

int8_t StemShell::process_line(const std::string& line) 
{
    // 每次執行新指令前，先清空上一次的錯誤
    _last_error_str.clear();

    auto args = tokenize(line);
    if (args.empty()) return 0; 

    std::string target = args[0];

    // 1. 檢查並執行微程式
    int32_t ret = _module_manager.call_micro_program(target, args);

    if (ret == vinc::SUCCESS) 
    {
        return 0; 
    } 
    else if (ret > -100 && ret < 0) 
    {
        // 🎯 寫入成員變數
        _last_error_str = "[" + target + "] failed: " + error_to_string(ret);
        return -1;
    }

    // 2. 檢查是否為已經在跑的 Node
    if (_module_manager.has_active_node(target)) 
    {
        int32_t node_ret = _module_manager.call_node_interface(target, args);
        if (node_ret != 0) 
        {
            _last_error_str = "Node '" + target + "' execution failed with return code: " + std::to_string(node_ret);
            return -1;
        }
        return 0;
    }

    // 3. 檢查是否為可以啟動的 Module Factory
    if (_module_manager.has_factory(target)) 
    {
        int32_t mod_ret = _module_manager.call_module_interface(target, args);
        if (mod_ret != 0) 
        {
            _last_error_str = "Module '" + target + "' execution failed with return code: " + std::to_string(mod_ret);
            return -1;
        }
        return 0;
    }

    // 4. 完全找不到對應指令
    _last_error_str = "Unknown command: '" + target + "'";
    //microHelp(args); 
    return -1;
}

int8_t StemShell::run_script(std::filesystem::path file_path)
{
    _script_terp.load_from_file(file_path);
    if(_script_terp.execute() != 0)
    {
        std::cout << "error to execute script : " << _script_terp.get_last_error() << std::endl;
        return -1;
    }
    return 0;
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

void StemShell::handle_history_scroll(bool is_up, std::string& buffer)
{
    if (_history.empty()) return;

    if (is_up) 
    {
        // 按「上」：往更舊的紀錄查
        if (_history_index > 0) {
            _history_index--;
        } else {
            return; // 已經是最頂了，不動
        }
    } 
    else 
    {
        // 按「下」：往最新的紀錄查
        if (_history_index < static_cast<int32_t>(_history.size())) {
            _history_index++;
        } else {
            return; // 已經是最底了，不動
        }
    }

    // 🧹 1. 物理擦除當前畫面上使用者已經打的字
    for (size_t i = 0; i < buffer.length(); ++i) {
        std::cout << "\b \b";
    }

    // 🔄 2. 更新 buffer 緩衝區
    if (_history_index == static_cast<int32_t>(_history.size())) {
        buffer.clear(); // 如果回到最底，就給一條乾淨的空行
    } else {
        buffer = _history[_history_index]; // 否則載入歷史紀錄
    }

    // 🚀 3. 把新指令噴上螢幕
    std::cout << buffer << std::flush;
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
    std::cout << "\n==== [vinc System Status] ====" << std::endl;
    
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

    std::cout << "VINC System Initialized. Type 'exit' to quit.\n";
    std::cout << _prompt << std::flush;

    while(_running)
    {
        char c = _terminal_guard.get_char();

        switch (c)
        {
            // 🎯 換行與回車：合併處理
            case '\n':
            case '\r':
            {

                std::cout << std::endl;
                if (!buffer.empty()) 
                {
                    process_line(buffer);
                }

                if (_history.empty() || _history.back() != buffer) 
                {
                    _history.push_back(buffer);
                }

                buffer.clear();buffer.clear();

                // 如果在執行完指令後 _running 變成 false，就不再印 prompt
                if (_running) std::cout << _prompt << std::flush;
                break;
            }

            // 🎯【新功能】Linux/macOS 方向鍵捕獲 (ESC 序列)
            case 27: 
            {
                char next1 = _terminal_guard.get_char();
                char next2 = _terminal_guard.get_char();

                if (next1 == '[') 
                {
                    if (next2 == 'A') {      // Esc[A = 上
                        handle_history_scroll(true, buffer);
                    } 
                    else if (next2 == 'B') { // Esc[B = 下
                        handle_history_scroll(false, buffer);
                    }
                }
                break;
            }

            // 🎯【新功能】Windows 方向鍵捕獲 (Extended Key 序列)
            case 0:
            case '\xE0': // 224 在 signed char 下對應的十六進位
            {
                char next = _terminal_guard.get_char();
                if (next == 72) {         // 72 = 上
                    handle_history_scroll(true, buffer);
                } 
                else if (next == 80) {    // 80 = 下
                    handle_history_scroll(false, buffer);
                }
                break;
            }

            // 🎯 Tab 鍵自動補全
            case '\t':
            {
                handle_tab_completion(buffer);
                break;
            }

            // 🎯 退格鍵：Windows 的 8 (\b) 與 Linux/macOS 常見的 127 (DEL)
            case 8:
            case 127:
            {
                if (!buffer.empty()) {
                    buffer.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
                break;
            }

            // 🎯 Ctrl+C：連續按 4 次強制物理超渡
            case 3:
            {
                ctrl_c_count++;
                if (ctrl_c_count >= 4)
                {
                    _running = false;
                }
                else
                {
                    std::cout << "\n[Interrupt] Press " << (4 - ctrl_c_count) 
                              << " more times to force exit." << std::endl;
                    std::cout << _prompt << buffer << std::flush; // 重新印出 prompt 與緩衝區內容
                }
                break;
            }


            // 🎯 一般可列印字元與非特定控制字元
            default:
            {
                // 💡 職人建議：未來如果要過濾掉不想要的控制字元（例如方向鍵的 Esc 序列），直接在這裡下 filter
                buffer += c;
                std::cout << c << std::flush;
                break;
            }
        }
    }
}

} // namespace vinc

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.resize(argc);

    for(int i = 0; i < argc; i++)
    {
        args.at(i) = argv[i];
    }

    vinc::StemShell shell;

    for(uint32_t i = 0; i < args.size(); i++)
    {
        if(args.at(i) == "-sc")
        {
            shell.run_script(args.at(i + 1));
        }
    }

    //TODO : run script

    shell.run();

    return 0;
}