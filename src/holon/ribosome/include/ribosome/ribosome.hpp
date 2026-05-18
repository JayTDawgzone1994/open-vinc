#pragma once

#include <iostream>
#include <string>
#include <filesystem>
#include <vector>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <map>

#include <ribosome/sha256.hpp>

namespace holon
{

#define ANSI_RES  "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_RED  "\033[1;31m"
#define ANSI_GRN  "\033[1;32m"
#define ANSI_YEL  "\033[1;33m"
#define ANSI_BLU  "\033[1;34m"
#define ANSI_WHT  "\033[1;37m"

struct RawMessage {
    std::string name;
    std::filesystem::path path;
};

struct VariableInfo {
    std::string type = "nan";
    std::string name = "nan";
    std::string const_value = "nan";
    bool is_array = false;
    bool is_func = false;
};

struct MessageInfo {
    std::string name;
    std::vector<VariableInfo> variables;
    uint64_t hash = 0;
};

class Ribosome
{

public:

    Ribosome() = default;
    ~Ribosome() = default;

    int8_t set_text_message_path(const std::filesystem::path& in_path);
    int8_t set_cpp_message_path(const std::filesystem::path& out_path);

    int8_t make_ligand();

private:

    int8_t load_all_msg_files();

    int8_t parse_message_file(const std::filesystem::path& path, MessageInfo& out_msg);

    void build_from_template(const MessageInfo& msg, const std::string& template_path, const std::string& output_path);

    bool is_fundamental_type(const std::string& type);

    int8_t check_type_validity(const std::string& type, bool& out_is_func);

    int8_t is_valid_identifier(const std::string& name, std::string& out_error_reason);

    int8_t check_global_collisions(const std::vector<MessageInfo>& all_msgs);

    void update_message_sha(MessageInfo& msg);

    void report_error(const std::filesystem::path& path, 
                            uint32_t line_num, 
                            const std::string& raw_line, 
                            const std::string& token, 
                            const std::string& message, 
                            const std::string& note);

    void replace_all(std::string& str, const std::string& from, const std::string& to);

    int8_t generate_all_messages_header(const std::vector<MessageInfo>& all_msgs);

    std::string generate_includes_code(const MessageInfo& msg);
    std::string generate_fields_code(const MessageInfo& msg);
    std::string generate_serialize_code(const MessageInfo& msg);
    std::string generate_deserialize_code(const MessageInfo& msg);
    std::string generate_data_to_string_code(const MessageInfo& msg);
    std::string generate_data_struct_to_string_code(const MessageInfo& msg);
    std::string generate_constants_code(const MessageInfo& msg);

    std::vector<RawMessage> _raw_message_files;
    //std::vector<VariableInfo> _vars;

    std::filesystem::path _in_path;
    std::filesystem::path _out_path;

    std::string _last_error_str;


};

} //namespace holon

