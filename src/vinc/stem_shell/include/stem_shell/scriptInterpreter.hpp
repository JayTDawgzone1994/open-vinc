#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace vinc {

// 前向宣告 StemShell，避免標頭檔循環引用
class StemShell;

class ScriptInterpreter {
public:
    // 建構子：綁定執行指令的 StemShell 主機
    explicit ScriptInterpreter(StemShell* shell);
    ~ScriptInterpreter() = default;

    // 從外部實體文字檔案載入腳本
    bool load_from_file(const std::filesystem::path& file_path);

    // 直接從記憶體字串陣列載入腳本
    bool load_from_lines(const std::vector<std::string>& lines);

    // 啟動虛擬機執行迴圈
    int8_t execute();

    // 取得腳本引擎內部的錯誤訊息
    std::string get_last_error() const { return _interpreter_error; }

private:
    // 🔍 第一階段：預先掃描並建立 if/while 的全域雙向跳躍地圖
    bool build_jump_map();

    // 🛠️ 輔助解析功能
    void trim_and_clean(std::string& line);
    std::string substitute_variables(const std::string& input);
    void handle_assignment(const std::string& var_line);
    bool evaluate_condition(const std::string& cond_line);

private:
    StemShell* _shell;                                   // 綁定的 Shell 主機指標
    std::vector<std::string> _lines;                    // 存放整份腳本的文字陣列
    std::unordered_map<std::string, std::string> _vars; // 🔑 變數口袋 ($var -> value)
    std::unordered_map<size_t, size_t> _jump_map;        // 🎯 核心跳躍地圖 (PC -> Target PC)
    std::string _interpreter_error;                     // 腳本引擎專屬錯誤訊息儲存槽
    bool _running = false;
};

} // namespace holon