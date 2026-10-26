#pragma once

// Build-time .msg compiler: parse definitions, resolve dependent schemas, then emit C++ facades.
// Generated fields bind to message storage instead of embedding process-specific wire pointers.

#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace vinc {
struct VariableInfo {
    std::string name;
    std::string type;
    unsigned dimensions = 0;
    bool builtin = false;
    std::optional<std::string> constant;
    std::uint32_t line = 0;
};

struct MessageInfo {
    std::string name;
    std::filesystem::path path;
    std::vector<VariableInfo> variables;
    std::uint64_t hash = 0;
};

class Ribosome {
public:
    std::int8_t set_text_message_path(const std::filesystem::path& path) {
        inputs_ = {path};
        return 0;
    }

    void add_text_message_path(const std::filesystem::path& path) {
        inputs_.push_back(path);
    }

    std::int8_t set_cpp_message_path(const std::filesystem::path& path) {
        output_ = path;
        return 0;
    }

    void set_template_path(const std::filesystem::path& path) {
        template_ = path;
    }

    std::int8_t make_ligand();

private:
    std::vector<MessageInfo> load();
    void resolve(std::vector<MessageInfo>& messages);
    std::string generate(const MessageInfo& message, const std::string& source);
    std::vector<std::filesystem::path> inputs_;
    std::filesystem::path output_;
    std::filesystem::path template_;
};
}
