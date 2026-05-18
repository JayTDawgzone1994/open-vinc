#include <ribosome/ribosome.hpp>

#include <generated/ribosome_config.hpp>

namespace vinc
{

int8_t Ribosome::set_text_message_path(const std::filesystem::path& in_path)
{
    std::cout << "Setting text message path to: " << in_path.string() << std::endl;
    //check path dir is exist
    if (!std::filesystem::exists(in_path))
    {
        _last_error_str = "Input path does not exist.";
        return -1;
    }
    _in_path = in_path;

    std::cout << "Set text message path to: " << _in_path.string() << std::endl;

    return 0;
}

int8_t Ribosome::set_cpp_message_path(const std::filesystem::path& out_path)
{
    std::error_code ec;

    // 1. 🧹 如果目標資料夾已經存在，先整包物理超渡（清空舊快取與殭屍檔案）
    if (std::filesystem::exists(out_path, ec))
    {
        std::filesystem::remove_all(out_path, ec);
        if (ec) [[unlikely]]
        {
            _last_error_str = "Failed to clear existing output directory: " + ec.message();
            return -1;
        }
    }
    else if (ec) [[unlikely]] // 預防 exists 函式本身因極端狀況（如上層權限崩潰）而失敗
    {
        _last_error_str = "Failed to check output directory existence: " + ec.message();
        return -1;
    }

    // 2. 📁 重新建立一個全新、乾乾淨淨的空資料夾
    std::filesystem::create_directories(out_path, ec);
    if (ec) [[unlikely]]
    {
        _last_error_str = "Failed to create output directory: " + ec.message();
        return -1;
    }

    // 3. 成功過關，寫入成員變數
    _out_path = out_path;

    std::cout << "Set C++ message output path to: " << _out_path.string() << std::endl;
    return 0;
}

int8_t Ribosome::make_ligand()
{
    if(load_all_msg_files() != 0)
    {
        std::cout << ANSI_RED << "[FAILED]" << ANSI_RES << _last_error_str << std::endl;
        return -1;
    }

    printf("Found %zu message files.\n", _raw_message_files.size());

    std::vector<MessageInfo> vars;
    for(const auto& msg : _raw_message_files)
    { 
        MessageInfo msg_info;
        if(parse_message_file(msg.path, msg_info))
        {
            printf("Failed to parse message file: %s\n", msg.path.string().c_str());
            printf("%s\n", _last_error_str.c_str());
            return -1;
        }
        printf("Parsed message [%s]: with %zu variables.\n", msg_info.name.c_str(), msg_info.variables.size());
        update_message_sha(msg_info);
        vars.push_back(msg_info);
    }

    std::cout << "Checking for hash collisions... ";
    if (check_global_collisions(vars) != 0) {
        std::cout << ANSI_RED << "[FAILED]" << ANSI_RES << std::endl;
        std::cerr << _last_error_str << std::endl;
        return -1;
    }
    std::cout << ANSI_GRN << "[OK]" << ANSI_RES << std::endl;

    for(const auto& msg : vars)
    {
        std::filesystem::path template_path = "D:\\dead-world\\src\\vinc\\ribosome\\data\\message_sample.txt";
        std::filesystem::path output_path = _out_path / (msg.name + ".hpp");
        build_from_template(msg, template_path.string(), output_path.string());
        printf("Generated message class for [%s] at: %s\n", msg.name.c_str(), output_path.string().c_str());
    }

    // =================================================================
    // 🎯【重構後調用】呼叫獨立打包函式
    // =================================================================
    if (generate_all_messages_header(vars) != 0)
    {
        std::cout << ANSI_RED << "[FAILED] " << ANSI_RES << _last_error_str << std::endl;
        return -1;
    }

    return 0;
}

int8_t Ribosome::load_all_msg_files()
{
    _raw_message_files.clear();
    std::error_code ec; // 用來接收錯誤狀態，不拋出異常

    // 1. 檢查路徑是否存在
    bool exists = std::filesystem::exists(_in_path, ec);
    if (ec || !exists) {
        _last_error_str = "Input path does not exist: " + (ec ? ec.message() : "Unknown error");
        return -1;
    }

    // 2. 檢查是否為目錄
    bool is_dir = std::filesystem::is_directory(_in_path, ec);
    if (ec || !is_dir) {
        _last_error_str = "Input path is not a directory: " + (ec ? ec.message() : "Unknown error");
        return -2;
    }

    // 3. 掃描目錄
    auto it = std::filesystem::directory_iterator(_in_path, ec);
    if (ec) {
        _last_error_str = "Failed to open directory iterator: " + ec.message();
        return -3;
    }

    // 使用非拋出異常的方式迭代
    for (const auto& entry : it) 
    {
        const auto& path = entry.path();

        std::cout << "Scanning file: " << path.string() << std::endl;
        
        // 檢查是否為一般檔案
        bool is_reg = std::filesystem::is_regular_file(path, ec);
        if (!ec && is_reg && path.extension() == ".msg") {
            RawMessage msg;
            msg.name = path.stem().string();
            msg.path = path;
            _raw_message_files.push_back(msg);
        }
    }

    return 0;
}

int8_t Ribosome::parse_message_file(const std::filesystem::path& path, MessageInfo& out_msg)
{
    out_msg.variables.clear();
    out_msg.name = path.stem().string();

    std::ifstream file(path);
    if (!file.is_open()) {
        _last_error_str = "Could not open message file: " + path.string();
        return -1;
    }

    std::string msg_name = path.stem().string();

    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t"));
        size_t last = s.find_last_not_of(" \t");
        if (last != std::string::npos) s.erase(last + 1);
    };

    std::string line;
    uint32_t line_count = 0;

    while (std::getline(file, line)) {

        line_count++;
        std::string raw_line = line;

        // 1. 基本清理
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty() || line.starts_with("//") || line.starts_with("#")) continue;

        std::stringstream ss(line);
        VariableInfo var;

        // 2. 抓取型別
        std::string raw_type;
        if (!(ss >> raw_type)) continue;

        // 檢查型別字串裡有沒有 [] (例如 float[])
        if (raw_type.find("[]") != std::string::npos) {
            var.is_array = true;
            // 移除型別中的 []
            size_t pos = raw_type.find('[');
            var.type = raw_type.substr(0, pos);
        } else {
            var.type = raw_type;
        }

        if(check_type_validity(var.type, var.is_func) != 0) 
        {
            report_error(path, line_count, raw_line, var.type, 
             "Unknown type '" + var.type + "'. It is neither a fundamental type nor a defined message.", 
             "Make sure to define the message type or use a valid fundamental type.");
            return -1;
        }

        if (var.type == msg_name) 
        {
            report_error(path, line_count, raw_line, var.type, 
             "Unknown type '" + var.type + "'. It is neither a fundamental type nor a defined message.", 
             "Make sure to define the message type or use a valid fundamental type.");
            return -1;
        }

        // 3. 處理剩餘部分 (名稱、陣列標誌、賦值)
        std::string remainder;
        std::getline(ss, remainder);

        // 移除分號
        size_t semi_pos = remainder.find(';');
        if (semi_pos != std::string::npos) remainder.erase(semi_pos);

        // 檢查名稱中是否帶有 [] (例如 float x[])
        if (remainder.find("[]") != std::string::npos) {
            var.is_array = true;
            // 移除剩餘字串中的 []
            size_t pos = remainder.find('[');
            size_t end_pos = remainder.find(']', pos);
            if (end_pos != std::string::npos) {
                remainder.erase(pos, end_pos - pos + 1);
            }
        }

        // 4. 分離名稱與賦值
        size_t equal_pos = remainder.find('=');
        if (equal_pos != std::string::npos) {
            var.name = remainder.substr(0, equal_pos);
            var.const_value = remainder.substr(equal_pos + 1);
        } else {
            var.name = remainder;
        }

        trim(var.name);
        trim(var.const_value);

        std::string error_reason;
        if (is_valid_identifier(var.name, error_reason) != 0)
        {
            report_error(path, line_count, raw_line, var.name, 
             "Invalid identifier '" + var.name + "'. " + error_reason, 
             "Make sure the variable name is a valid C++ identifier and not a reserved keyword.");
            return -1;
        }

        if (!var.name.empty()) {
            out_msg.variables.push_back(var);
        }
    }

    file.close();
    return 0;
}

void Ribosome::build_from_template(const MessageInfo& msg, const std::string& template_path, const std::string& output_path) 
{
    std::ifstream t_file(template_path);
    std::ofstream o_file(output_path);

    if (!t_file.is_open() || !o_file.is_open()) {
        // 噴個偏執狂必備的 error
        return;
    }

    std::map<std::string, std::string> block_macros = 
    {
        {"${DO_INCLUDE_DEPENDENT_MESSAGES}", generate_includes_code(msg)},
        {"${DO_ADD_MESSAGE_FIELDS}",         generate_fields_code(msg)},
        {"${DO_MESSAGE_SERIALIZE_FIELDS}",   generate_serialize_code(msg)},
        {"${DO_MESSAGE_DESERIALIZE_FIELDS}", generate_deserialize_code(msg)},
        {"${DO_MESSAGE_TO_STRING_FIELDS}",   generate_data_to_string_code(msg)},
        {"${DO_DATA_STRUCT_TO_STRING}",      generate_data_struct_to_string_code(msg)},
        {"${DO_ADD_CONSTANTS_DEFINITIONS}",  generate_constants_code(msg)}
    };

    std::stringstream hash_ss;
    hash_ss << "0x" << std::hex << msg.hash << "ULL";

    std::map<std::string, std::string> inline_macros = {
        {"${MESSAGE_NAME}",      msg.name},
        {"${MESSAGE_TYPE_HASH}", hash_ss.str()}
    };

    // B. 開始「讀一行、噴一行」
    std::string line;
    while (std::getline(t_file, line)) 
    {
        bool processed = false;

        // 首先檢查是不是「區塊標籤」
        for (const auto& [tag, content] : block_macros) {
            if (line.find(tag) != std::string::npos) {
                o_file << content; // 噴出預先產生好的區塊
                processed = true;
                break;
            }
        }

        if (processed) continue;

        // 如果不是區塊標籤，檢查有沒有「嵌入型標籤」需要替換
        for (const auto& [tag, value] : inline_macros) {
            if (line.find(tag) != std::string::npos) {
                replace_all(line, tag, value);
            }
        }

        // 最後把處理完（或沒變）的一行噴出去
        o_file << line << "\n";
    }

    t_file.close();
    o_file.close();
}



bool Ribosome::is_fundamental_type(const std::string& type) {
    static const std::unordered_set<std::string> fundamentals = {
        "bool", "char", "int", "float", "double",
        "long", "short",
        "int8_t", "uint8_t", "int16_t", "uint16_t",
        "int32_t", "uint32_t", "int64_t", "uint64_t",
        "int8", "uint8", "int16", "uint16",
        "int32", "uint32", "int64", "uint64",
        "size_t", "string"
    };
    return fundamentals.find(type) != fundamentals.end();
}


int8_t Ribosome::check_type_validity(const std::string& type, bool& out_is_func)
{
// 1. 首先檢查是否為 C++ 基礎型別
    if (is_fundamental_type(type)) 
    {
        out_is_func = true;
        return 0; // 沒問題
    }

    // 2. 如果不是基礎型別，檢查是否在我們掃描到的訊息清單中
    // 假設 _raw_message_files 已經在 load_all_msg_files() 填好了
    for (const auto& msg : _raw_message_files) 
    {
        if (msg.name == type) 
        {
            out_is_func = false; // 找到了，是自定義訊息型別
            return 0; // 沒問題
        }
    }

    // 3. 都找不到，噴錯
    _last_error_str = "Error: Unknown type '" + type + "'. It is neither a fundamental type nor a defined message.";
    return -1;
}

int8_t Ribosome::is_valid_identifier(const std::string& name, std::string& out_error_reason) {
    if (name.empty()) {
        out_error_reason = "identifier is empty";
        return -1;
    }

    // 1. 檢查開頭字元：只能是英文字母或下底線，不能是數字
    if (!std::isalpha(name[0]) && name[0] != '_') {
        out_error_reason = "identifier must start with a letter or underscore";
        return -1;
    }

    // 2. 檢查後續字元：只能是字母、數字、下底線
    for (char c : name) {
        if (!std::isalnum(c) && c != '_') {
            out_error_reason = "identifier contains invalid character '" + std::string(1, c) + "'";
            return -1;
        }
    }

    // 3. 檢查是否為 C++ 關鍵字 (保留字)
    static const std::unordered_set<std::string> keywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
        "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "reflexpr", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };

    if (keywords.find(name) != keywords.end()) {
        out_error_reason = "'" + name + "' is a C++ reserved keyword";
        return -1;
    }

    return 0;
}

void Ribosome::report_error(const std::filesystem::path& path, 
                            uint32_t line_num, 
                            const std::string& raw_line, 
                            const std::string& token, 
                            const std::string& message, 
                            const std::string& note)
{
    std::stringstream err;
    size_t pos = raw_line.find(token);
    if (pos == std::string::npos) pos = 0; // 防呆

    // 1. 標題列 [路徑]:[行號]:[字元偏移]: error: [訊息]
    err << ANSI_BOLD << path.string() << ":" << line_num << ":" << (pos + 1) << ": "
        << ANSI_RED << "error: " << ANSI_RES
        << ANSI_BOLD << message << ANSI_RES << "\n";
    
    // 2. 原始碼視覺化
    // 這裡用 std::setw 或是手動空格對齊行號
    err << "    " << line_num << " | " << raw_line << "\n";
    err << "      | " << ANSI_RED << ANSI_BOLD;
    
    // 3. 自動畫波浪線 (長度與 token 一致)
    for (size_t i = 0; i < pos; ++i) err << " ";
    err << "^";
    if (token.length() > 1) {
        for (size_t i = 1; i < token.length(); ++i) err << "~";
    }
    err << ANSI_RES << "\n";

    // 4. 選配 Note
    if (!note.empty()) {
        err << ANSI_YEL << "note: " << ANSI_RES << note << "\n";
    }

    _last_error_str = err.str();
}


void Ribosome::update_message_sha(MessageInfo& msg) {
    crypto::SHA256 sha;

    sha.update(msg.name);

    for (const auto& var : msg.variables) {
        sha.update(var.type);
        sha.update(var.name);
        sha.update(var.is_array ? "arr:1" : "arr:0");
        sha.update(var.const_value);
    }

    auto digest = sha.digest();
    
    msg.hash = 0;
    for(int i=0; i<8; ++i) {
        msg.hash |= (static_cast<uint64_t>(digest[i]) << (56 - i * 8));
    }

    std::cout << "SHA-256: " << crypto::SHA256::to_hex(digest) << std::endl;
}

int8_t Ribosome::check_global_collisions(const std::vector<MessageInfo>& all_msgs)
{
    // hash -> message_name
    std::unordered_map<uint64_t, std::string> seen_hashes;

    for (const auto& msg : all_msgs) 
    {
        auto it = seen_hashes.find(msg.hash);
        
        if (it != seen_hashes.end()) 
        {
            std::stringstream err;
            err << ANSI_RED << ANSI_BOLD << "fatal error: " << ANSI_RES
                << "hash collision detected in global scope!\n"
                << "--------------------------------------------------\n"
                << ANSI_WHT << "Message A: " << ANSI_RES << ANSI_YEL << it->second << ANSI_RES << "\n"
                << ANSI_WHT << "Message B: " << ANSI_RES << ANSI_YEL << msg.name << ANSI_RES << "\n"
                << ANSI_WHT << "Common Fingerprint: " << ANSI_RES << ANSI_BOLD << "0x" 
                << std::hex << std::setw(16) << std::setfill('0') << msg.hash << ANSI_RES << "\n"
                << "--------------------------------------------------\n"
                << ANSI_YEL << "note: " << ANSI_RES 
                << "This is extremely rare. Try renaming a variable or changing member order to resolve.\n";

            _last_error_str = err.str();
            return -1;
        }
        
        // 沒事，記錄下來繼續跑
        seen_hashes[msg.hash] = msg.name;
    }
    return 0; // 全部檢查完沒問題
}

void Ribosome::replace_all(std::string& str, const std::string& from, const std::string& to)
{
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

int8_t Ribosome::generate_all_messages_header(const std::vector<MessageInfo>& all_msgs)
{
    std::filesystem::path all_msg_path = _out_path / "all_messages.hpp";
    std::ofstream all_msg_file(all_msg_path);
    if (!all_msg_file.is_open())
    {
        _last_error_str = "Failed to open all_messages.hpp for writing.";
        return -1;
    }

    // 寫入 Umbrella Header 標準防護外殼
    all_msg_file << "#pragma once\n\n";
    all_msg_file << "/* ===================================================================== */\n";
    all_msg_file << "/* 🧬 AUTOMATICALLY GENERATED BY RIBOSOME - DO NOT EDIT DIRECTLY         */\n";
    all_msg_file << "/* ===================================================================== */\n\n";

    // 點名所有解析成功的 message，吐出對應的 include 程式碼
    for (const auto& msg : all_msgs)
    {
        all_msg_file << "#include <vinc_message/" << msg.name << ".hpp>\n";
    }
    all_msg_file.close();

    printf("Generated master message header [all_messages.hpp] at: %s\n", all_msg_path.string().c_str());
    return 0;
}

std::string Ribosome::generate_includes_code(const MessageInfo& msg)
{
    std::stringstream ss;
    std::unordered_set<std::string> includes;

    for (const auto& var : msg.variables) 
    {
        if (var.is_func == false)
        {
            ss << "#include <vinc_message/" << var.type << ".hpp>n";
        }
    }

    return ss.str();
}

std::string Ribosome::generate_fields_code(const MessageInfo& msg)
{
    std::stringstream ss;
    for (const auto& var : msg.variables)
    {
        // 偏執檢查：常量不應該出現在 Data_t 裡，因為它們不需要被序列化
        if (var.const_value != "nan") continue;

        if (var.is_array) 
        {
            // 如果是陣列，就用我們心愛的 vector
            ss << "    std::vector<" << var.type << "> " << var.name << ";\n";
        } 
        else 
        {
            // 普通型別
            ss << "    " << var.type << " " << var.name << ";\n";
        }
    }
    return ss.str();
}

std::string Ribosome::generate_serialize_code(const MessageInfo& msg)
{
    std::stringstream ss;
    for (const auto& var : msg.variables)
    {
        // 偏執檢查：常量不參與傳輸
        if (var.const_value != "nan") continue;

        if (var.is_array) 
        {
            ss << "        {\n";
            ss << "            uint32_t sz = static_cast<uint32_t>(_message_data." << var.name << ".size());\n";
            ss << "            push_raw(buffer, sz);\n";
            
            if (var.is_func) { 
                // A. 原生型別 (int, float...)：直接整塊記憶體塞進去，效率最高
                ss << "            if (sz > 0) {\n";
                ss << "                const uint8_t* p = reinterpret_cast<const uint8_t*>(_message_data." << var.name << ".data());\n";
                ss << "                buffer.insert(buffer.end(), p, p + (sz * sizeof(" << var.type << ")));\n";
                ss << "            }\n";
            } else {
                // B. 自定義訊息：必須逐一呼叫成員的 serialize (因為成員可能也帶 vector)
                ss << "            for (const auto& item : _message_data." << var.name << ") {\n";
                ss << "                item.serialize(buffer);\n";
                ss << "            }\n";
            }
            ss << "        }\n";
        } 
        else 
        {
            if (var.is_func) {
                // 單一原生型別
                ss << "        push_raw(buffer, _message_data." << var.name << ");\n";
            } else {
                // 單一自定義訊息
                ss << "        _message_data." << var.name << ".serialize(buffer);\n";
            }
        }
    }
    return ss.str();
}

std::string Ribosome::generate_deserialize_code(const MessageInfo& msg)
{
std::stringstream ss;
    for (const auto& var : msg.variables)
    {
        if (var.const_value != "nan") continue;

        if (var.is_array) 
        {
            ss << "        {\n";
            ss << "            uint32_t sz = 0;\n";
            // 使用你的 pop_raw 檢查長度
            ss << "            if (!pop_raw(buffer, offset, sz)) return false;\n";
            ss << "            _message_data." << var.name << ".resize(sz);\n";
            
            if (var.is_func) {
                ss << "            if (sz > 0) {\n";
                ss << "                size_t byte_count = sz * sizeof(" << var.type << ");\n";
                ss << "                if (offset + byte_count > buffer.size()) return false;\n";
                // 偏執優化：原生型別直接用 memcpy (或 std::copy) 搬整塊
                ss << "                std::memcpy(_message_data." << var.name << ".data(), &buffer[offset], byte_count);\n";
                ss << "                offset += byte_count;\n";
                ss << "            }\n";
            } else {
                ss << "            for (uint32_t i = 0; i < sz; ++i) {\n";
                // 遞迴呼叫巢狀訊息的反序列化
                ss << "                if (!_message_data." << var.name << "[i].deserialize(buffer, offset)) return false;\n";
                ss << "            }\n";
            }
            ss << "        }\n";
        } 
        else 
        {
            if (var.is_func) {
                ss << "        if (!pop_raw(buffer, offset, _message_data." << var.name << ")) return false;\n";
            } else {
                // 單一自定義訊息同樣使用帶 offset 的版本
                ss << "        if (!_message_data." << var.name << ".deserialize(buffer, offset)) return false;\n";
            }
        }
    }
    return ss.str();
}

std::string Ribosome::generate_data_to_string_code(const MessageInfo& msg)
{
    std::stringstream ss;
    ss << "        ss << \"" << msg.name << " { \";\n";

    bool first = true;
    for (const auto& var : msg.variables)
    {
        if (var.const_value != "nan") continue;

        if (!first) ss << "        ss << \", \";\n";
        first = false;

        ss << "        ss << \"" << var.name << ": \";\n";

        if (var.is_array) 
        {
            ss << "        ss << \"[\";\n";
            ss << "        for (size_t i = 0; i < _message_data." << var.name << ".size(); ++i) {\n";
            ss << "            if (i > 0) ss << \", \";\n";
            
            if (var.is_func) {
                ss << "            ss << _message_data." << var.name << "[i];\n";
            } else {
                // 關鍵：呼叫該型別 Class 的 Static 函式
                ss << "            ss << " << var.type << "::to_string(_message_data." << var.name << "[i]);\n";
            }
            ss << "        }\n";
            ss << "        ss << \"]\";\n";
        } 
        else 
        {
            if (var.is_func) {
                ss << "        ss << _message_data." << var.name << ";\n";
            } else {
                // 關鍵：呼叫該型別 Class 的 Static 函式
                ss << "        ss << " << var.type << "::to_string(_message_data." << var.name << ");\n";
            }
        }
    }
    ss << "        ss << \" }\";\n";
    return ss.str();
}

std::string Ribosome::generate_data_struct_to_string_code(const MessageInfo& msg)
{
    std::stringstream ss;
    ss << "        ss << \"" << msg.name << " { \";\n";

    bool first = true;
    for (const auto& var : msg.variables)
    {
        if (var.const_value != "nan") continue;

        if (!first) ss << "        ss << \", \";\n";
        first = false;

        ss << "        ss << \"" << var.name << ": \";\n";

        if (var.is_array) 
        {
            ss << "        ss << \"[\";\n";
            ss << "        for (size_t i = 0; i < data." << var.name << ".size(); ++i) {\n";
            ss << "            if (i > 0) ss << \", \";\n";
            
            if (var.is_func) {
                ss << "            ss << data." << var.name << "[i];\n";
            } else {
                // 關鍵：呼叫該型別 Class 的 Static 函式
                ss << "            ss << " << var.type << "::to_string(data." << var.name << "[i]);\n";
            }
            ss << "        }\n";
            ss << "        ss << \"]\";\n";
        } 
        else 
        {
            if (var.is_func) {
                ss << "        ss << data." << var.name << ";\n";
            } else {
                // 關鍵：呼叫該型別 Class 的 Static 函式
                ss << "        ss << " << var.type << "::to_string(data." << var.name << ");\n";
            }
        }
    }
    ss << "        ss << \" }\";\n";
    return ss.str();
}

std::string Ribosome::generate_constants_code(const MessageInfo& msg)
{
    std::stringstream ss;
    bool has_const = false;

    for (const auto& var : msg.variables)
    {
        // 只有 const_value 不是 "nan" 的變數才視為常量
        if (var.const_value == "nan") continue;

        if (!has_const) {
            ss << "    // --- Constants Definitions ---\n";
            has_const = true;
        }

        // 根據型別做一點小小的優化處理
        if (var.type == "float" && var.const_value.find('f') == std::string::npos) {
            // 如果是 float 且沒加 f，幫它補上，避免編譯器抱怨 double 轉 float
            ss << "    static constexpr " << var.type << " " << var.name 
               << " = " << var.const_value << "f;\n";
        } 
        else {
            ss << "    static constexpr " << var.type << " " << var.name 
               << " = " << var.const_value << ";\n";
        }
    }

    if (has_const) ss << "\n";
    return ss.str();
}

}

int main()
{
    vinc::Ribosome ribosome;
    std::cout << "Starting Ribosome...\n";
    if(ribosome.set_text_message_path(vinc::kDefaultInputPath) != 0)
    {
        std::cout << ANSI_RED << "[FAILED]" << ANSI_RES << std::endl;
        return -1;
    }
    ribosome.set_cpp_message_path(vinc::kDefaultOutputPath);

    return ribosome.make_ligand();

}