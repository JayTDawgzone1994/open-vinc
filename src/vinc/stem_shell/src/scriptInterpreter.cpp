#include <stem_shell/scriptInterpreter.hpp>
#include <stem_shell/stem_shell.hpp> // 引入以便呼叫 process_line 與 get_last_error
#include <fstream>
#include <sstream>
#include <iostream>
#include <stack>
#include <algorithm>

namespace vinc {

ScriptInterpreter::ScriptInterpreter(StemShell* shell)
    : _shell(shell)
{
}

bool ScriptInterpreter::load_from_file(const std::filesystem::path& file_path)
{
    _lines.clear();
    _interpreter_error.clear();

    std::ifstream file(file_path);
    if (!file.is_open()) {
        _interpreter_error = "Cannot open script file: " + file_path.string();
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        _lines.push_back(line);
    }
    file.close();

    return build_jump_map();
}

bool ScriptInterpreter::load_from_lines(const std::vector<std::string>& lines)
{
    _lines = lines;
    _interpreter_error.clear();
    return build_jump_map();
}

bool ScriptInterpreter::build_jump_map()
{
    _jump_map.clear();
    std::stack<size_t> if_stack;
    std::stack<size_t> while_stack;

    for (size_t i = 0; i < _lines.size(); ++i) 
    {
        std::string line = _lines[i];
        trim_and_clean(line);

        if (line.empty()) continue;

        if (line.starts_with("if ")) 
        {
            if_stack.push(i);
        } 
        else if (line == "endif") 
        {
            if (if_stack.empty()) [[unlikely]] {
                _interpreter_error = "Syntax Error: 'endif' without matching 'if' at line " + std::to_string(i + 1);
                return false;
            }
            size_t if_pc = if_stack.top();
            if_stack.pop();
            _jump_map[if_pc] = i;  // if 失敗時跳到 endif
            _jump_map[i] = if_pc;  // 雙向紀錄
        } 
        else if (line.starts_with("while ")) 
        {
            while_stack.push(i);
        } 
        else if (line == "endwhile") 
        {
            if (while_stack.empty()) [[unlikely]] {
                _interpreter_error = "Syntax Error: 'endwhile' without matching 'while' at line " + std::to_string(i + 1);
                return false;
            }
            size_t while_pc = while_stack.top();
            while_stack.pop();
            _jump_map[while_pc] = i; // while 結束時跳出到 endwhile 之後
            _jump_map[i] = while_pc; // endwhile 無條件跳回 while 開頭
        }
    }

    if (!if_stack.empty()) [[unlikely]] {
        _interpreter_error = "Syntax Error: Missing 'endif' for 'if' at line " + std::to_string(if_stack.top() + 1);
        return false;
    }
    if (!while_stack.empty()) [[unlikely]] {
        _interpreter_error = "Syntax Error: Missing 'endwhile' for 'while' at line " + std::to_string(while_stack.top() + 1);
        return false;
    }

    return true;
}

int8_t ScriptInterpreter::execute()
{
    if (!_shell) return -1;
    _running = true;
    size_t pc = 0; // Program Counter 虛擬計數器（代表當前行號）

    while (pc < _lines.size() && _running) 
    {
        std::string line = _lines[pc];
        trim_and_clean(line);

        // 跳過空行與純註解行
        if (line.empty()) {
            pc++;
            continue;
        }

        // 1. 處理變數賦值 (例如: $loop_count = 0)
        if (line.starts_with("$") && line.find('=') != std::string::npos) 
        {
            handle_assignment(line);
            pc++;
        }
        // 2. 處理 IF 分支控制
        else if (line.starts_with("if ")) 
        {
            if (!evaluate_condition(line.substr(3))) {
                // 條件為假：透過地圖瞬間超渡到對應的 endif 行
                pc = _jump_map[pc] + 1;
            } else {
                pc++; // 條件為真：順著往下走
            }
        }
        else if (line == "endif") 
        {
            pc++; // 僅作為邊界標記，直接通過
        }
        // 3. 處理 WHILE 迴圈控制
        else if (line.starts_with("while ")) 
        {
            if (!evaluate_condition(line.substr(6))) {
                // 條件不滿足：跳出迴圈，移動到 endwhile 的下一行
                pc = _jump_map[pc] + 1;
            } else {
                pc++; // 條件滿足：進入迴圈本體
            }
        }
        else if (line == "endwhile") 
        {
            // 抵達迴圈底部，無條件瞬移回 while 開頭重新評估條件
            pc = _jump_map[pc];
        }
        // 4. 普通系統核心指令：進行變數代換後，直接對接給主機的 process_line
        else 
        {
            std::string ready_cmd = substitute_variables(line);
            if (_shell->process_line(ready_cmd) != 0) 
            {
                // 完美串接：直接向主機調用 get_last_error()
                _interpreter_error = "Runtime Error at line " + std::to_string(pc + 1) + ": " + _shell->get_last_error();
                _running = false;
                return -1;
            }
            pc++;
        }
    }

    return 0;
}

void ScriptInterpreter::handle_assignment(const std::string& var_line)
{
    size_t eq_pos = var_line.find('=');
    std::string var_name = var_line.substr(0, eq_pos);
    std::string expr = var_line.substr(eq_pos + 1);

    trim_and_clean(var_name);
    trim_and_clean(expr);

    // 去除變數名稱前面的 $ 符號，統一格式
    if (var_name.starts_with("$")) var_name = var_name.substr(1);

    // 先對等號右邊的表達式進行全域變數替換
    std::string substituted = substitute_variables(expr);

    // 內建極輕量計算機：支援基礎整數加減法 (如: 0 + 1)
    if (substituted.find('+') != std::string::npos) 
    {
        size_t plus_pos = substituted.find('+');
        std::string left = substituted.substr(0, plus_pos);
        std::string right = substituted.substr(plus_pos + 1);
        trim_and_clean(left); trim_and_clean(right);
        try {
            int32_t res = std::stoi(left) + std::stoi(right);
            _vars[var_name] = std::to_string(res);
        } catch (...) { _vars[var_name] = substituted; } // 失敗則退化為普通字串儲存
    } 
    else if (substituted.find('-') != std::string::npos) 
    {
        size_t minus_pos = substituted.find('-');
        std::string left = substituted.substr(0, minus_pos);
        std::string right = substituted.substr(minus_pos + 1);
        trim_and_clean(left); trim_and_clean(right);
        try {
            int32_t res = std::stoi(left) - std::stoi(right);
            _vars[var_name] = std::to_string(res);
        } catch (...) { _vars[var_name] = substituted; }
    } 
    else 
    {
        // 純粹數值或字串賦值
        _vars[var_name] = substituted;
    }
}

bool ScriptInterpreter::evaluate_condition(const std::string& cond_line)
{
    std::string cleaned = cond_line;
    trim_and_clean(cleaned);

    // 支援的工業比較遮罩
    static const std::vector<std::string> operators = { "==", "!=", "<=", ">=", "<", ">" };
    std::string op = "";
    size_t op_pos = std::string::npos;

    for (const auto& possible_op : operators) {
        op_pos = cleaned.find(possible_op);
        if (op_pos != std::string::npos) {
            op = possible_op;
            break;
        }
    }

    if (op_pos == std::string::npos) return false; // 找不到運算子，直接判定條件失敗

    std::string left = cleaned.substr(0, op_pos);
    std::string right = cleaned.substr(op_pos + op.length());
    
    left = substitute_variables(left);
    right = substitute_variables(right);
    trim_and_clean(left); trim_and_clean(right);

    // 嘗試進行數位整數比對，若非數字則自動降格為普通字串字典序比對
    bool is_numeric = !left.empty() && !right.empty() && 
                      std::all_of(left.begin(), left.end(), [](char c){ return std::isdigit(c) || c == '-'; }) &&
                      std::all_of(right.begin(), right.end(), [](char c){ return std::isdigit(c) || c == '-'; });

    if (is_numeric) 
    {
        int32_t l_val = std::stoi(left);
        int32_t r_val = std::stoi(right);
        if (op == "== ") return l_val == r_val; // 防呆
        if (op == "==")  return l_val == r_val;
        if (op == "!=")  return l_val != r_val;
        if (op == "<=")  return l_val <= r_val;
        if (op == ">=")  return l_val >= r_val;
        if (op == "<")   return l_val < r_val;
        if (op == ">")   return l_val > r_val;
    } 
    else 
    {
        if (op == "==")  return left == right;
        if (op == "!=")  return left != right;
        if (op == "<=")  return left <= right;
        if (op == ">=")  return left >= right;
        if (op == "<")   return left < right;
        if (op == ">")   return left > right;
    }

    return false;
}

std::string ScriptInterpreter::substitute_variables(const std::string& input)
{
    std::stringstream result;
    std::string word;
    std::stringstream ss(input);

    // 逐字元掃描，精準提取帶有 $ 的關鍵字
    for (size_t i = 0; i < input.length(); ++i) 
    {
        if (input[i] == '$') 
        {
            std::string var_name = "";
            i++; // 跳過 '$' 本身
            while (i < input.length() && (std::isalnum(input[i]) || input[i] == '_')) {
                var_name += input[i];
                i++;
            }
            // 從口袋搜尋變數，找不到就原樣吐回空字串
            if (_vars.find(var_name) != _vars.end()) {
                result << _vars[var_name];
            }
            i--; // 退回一格，補償外層迴圈的 i++
        } 
        else 
        {
            result << input[i];
        }
    }

    return result.str();
}

void ScriptInterpreter::trim_and_clean(std::string& line)
{
    // 1. 物理抹除 '//' 或 '#' 後面的整行註解
    size_t comment_pos = line.find("//");
    if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
    comment_pos = line.find('#');
    if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);

    // 2. 去除前後髒空格 / 換行符號
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    size_t last = line.find_last_not_of(" \t\r\n");
    if (last != std::string::npos) {
        line.erase(last + 1);
    }
}

} // namespace holon