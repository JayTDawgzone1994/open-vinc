// Ribosome pipeline: validate .msg syntax, canonicalize types, resolve dependencies,
// fingerprint schemas, and publish a complete set of generated headers from a staging directory.

#include <ribosome/ribosome.hpp>
#include <ribosome/sha256.hpp>
#include <ribosome_config.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vinc {
namespace {
namespace fs = std::filesystem;

// Compiler input budgets, independent of runtime message capacity.
constexpr std::uintmax_t max_definition_bytes = 1024 * 1024;
constexpr std::size_t max_declaration_bytes = 4096;
constexpr std::size_t max_fields_per_message = 4096;
constexpr unsigned max_array_dimensions = 64;
constexpr unsigned max_dependency_depth = 64;
constexpr unsigned max_initial_schema_depth = 64;
constexpr std::size_t floating_literal_buffer_bytes = 128;

// Schema identity is the leading 64 bits of the digest, in big-endian order.
constexpr unsigned schema_hash_bytes = 8;
enum class VisitState {
    Unvisited,
    Visiting,
    Complete
};

// Diagnostics use UTF-8; filesystem operations keep the native path representation.
std::string path_text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path_text(path));
    }

    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (file.bad()) {
        throw std::runtime_error("Failed to read file: " + path_text(path));
    }

    return text;
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    if (!file) {
        throw std::runtime_error("Failed to write generated header: " + path_text(path));
    }
}

std::string trim(std::string text) {
    const auto start = text.find_first_not_of(" \r\n\t");
    if (start == std::string::npos) {
        return {};
    }

    return text.substr(start, text.find_last_not_of(" \r\n\t") - start + 1);
}

bool valid_identifier(const std::string& name) {
    static const std::regex pattern("[A-Za-z][A-Za-z0-9_]*");
    static const std::set<std::string> reserved{
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "asm",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "compl",
        "concept",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "not_eq",
        "nullptr",
        "operator",
        "or",
        "or_eq",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
        "xor_eq",
        "all_messages",
        "schema",
        "data",
        "snapshot",
        "usage",
        "compact",
        "serialize",
        "deserialize",
        "assign",
        "from_bytes",
        "to_string",
        "type_hash",
        "msg_name_str",
        "get_type_hash_static",
        "message_name",
        "binding",
        "binding_",
        "data_",
        "topic_name",
        "publisher_name",
        "get_message_timestamp",
        "get_message_monotonic_timestamp",
        "get_topic_count",
        "publisher_instance",
        "message",
        "BaseMessage",
        "MessageArray",
        "MessageArrayView",
        "DataRef",
        "ReadOnly",
        "Ribosome",
        "std",
    };
    return std::regex_match(name, pattern) && name.find("__") == std::string::npos &&
           !name.starts_with("ribosome_") && !reserved.contains(name);
}

// These names share namespace vinc with generated classes. Field names live in
// another scope and need not inherit this additional restriction.
bool reserved_message_name(const std::string& name) {
    static const std::set<std::string> names{
        "Result", "Error", "ErrorRecord", "ErrorDomain", "NativeError", "NativeErrorSource",
        "ErrorCodeTraits", "ErrorOperationTraits", "ErrorCodeEnum", "ErrorOperationEnum",
        "config", "detail", "message_bus", "make_message", "make_error", "error_to_string",
        "wrap_error", "contract_violation", "format_utc", "unix_nanoseconds"
    };
    return names.contains(name);
}

// Resolve source aliases to explicit widths before hashing or emitting C++.
// For example, the schema meaning of long is int64 even on an OS where native long is 32-bit.
std::string canonical_builtin(std::string type) {
    if (type.starts_with("std::")) {
        type.erase(0, 5);
    }

    static const std::map<std::string, std::string> aliases{
        {"int", "std::int32_t"},
        {"short", "std::int16_t"},
        {"long", "std::int64_t"},
        {"char", "std::int8_t"},
        {"size_t", "std::uint64_t"},
        {"int8", "std::int8_t"},
        {"uint8", "std::uint8_t"},
        {"int16", "std::int16_t"},
        {"uint16", "std::uint16_t"},
        {"int32", "std::int32_t"},
        {"uint32", "std::uint32_t"},
        {"int64", "std::int64_t"},
        {"uint64", "std::uint64_t"},
        {"int8_t", "std::int8_t"},
        {"uint8_t", "std::uint8_t"},
        {"int16_t", "std::int16_t"},
        {"uint16_t", "std::uint16_t"},
        {"int32_t", "std::int32_t"},
        {"uint32_t", "std::uint32_t"},
        {"int64_t", "std::int64_t"},
        {"uint64_t", "std::uint64_t"},
        {"float", "float"},
        {"double", "double"},
        {"bool", "bool"},
        {"string", "message::MessageString"},
    };
    const auto found = aliases.find(type);
    return found == aliases.end() ? "" : found->second;
}

[[noreturn]] void error(const MessageInfo& message, std::uint32_t line, const std::string& reason) {
    throw std::runtime_error(path_text(message.path) + ":" + std::to_string(line) + ": error: " + reason);
}

unsigned integer_width(const std::string& type) {
    if (type.find("8_t") != std::string::npos) {
        return 8;
    }

    if (type.find("16_t") != std::string::npos) {
        return 16;
    }

    if (type.find("32_t") != std::string::npos) {
        return 32;
    }

    return 64;
}

template<class Float>
std::string emit_floating_constant(const MessageInfo& message, const VariableInfo& field,
                                   const std::string& text) {
    Float value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        error(message, field.line, "invalid or out-of-range floating constant");
    }

    // Scientific notation is always a C++ floating literal, even for inputs
    // such as 010. Round-trip digits preserve the declared type and negative zero.
    std::array<char, floating_literal_buffer_bytes> buffer{};
    const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         std::chars_format::scientific,
                                         std::numeric_limits<Float>::max_digits10 - 1);
    if (formatted.ec != std::errc{}) {
        error(message, field.line, "failed to format floating constant");
    }

    auto literal = std::string(buffer.data(), formatted.ptr);
    if constexpr (std::is_same_v<Float, float>) {
        literal += 'f';
    }
    return literal;
}

std::string floating_constant(const MessageInfo& message, const VariableInfo& field, std::string text) {
    if (text == "nan") {
        return "std::numeric_limits<" + field.type + ">::quiet_NaN()";
    }

    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
        text.pop_back();
    }

    if (field.type == "float") {
        return emit_floating_constant<float>(message, field, text);
    }

    return emit_floating_constant<double>(message, field, text);
}

std::string string_constant(const MessageInfo& message, const VariableInfo& field, const std::string& text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        error(message, field.line, "invalid string constant");
    }

    std::string bytes;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        char byte = text[i];
        if (byte == '\\') {
            ++i;
            if (i + 1 >= text.size()) {
                error(message, field.line, "invalid string escape");
            }

            switch (text[i]) {
            case 'n':
                byte = '\n';
                break;
            case 'r':
                byte = '\r';
                break;
            case 't':
                byte = '\t';
                break;
            case '0':
                byte = '\0';
                break;
            case '"':
                byte = '"';
                break;
            case '\\':
                byte = '\\';
                break;
            default:
                error(message, field.line, "invalid string escape");
            }
        } else if (byte == '"' || byte == '\r' || byte == '\n') {
            error(message, field.line, "invalid string constant");
        }
        bytes.push_back(byte);
    }

    // Fixed three-digit octal escapes preserve each input byte independently
    // of the compiler's execution charset and prevent a following digit joining an escape.
    std::string literal = "std::string_view{\"";
    for (const unsigned char byte : bytes) {
        literal += '\\';
        literal += static_cast<char>('0' + ((byte >> 6) & 7));
        literal += static_cast<char>('0' + ((byte >> 3) & 7));
        literal += static_cast<char>('0' + (byte & 7));
    }
    literal += "\", " + std::to_string(bytes.size()) + "}";
    return literal;
}

std::string integer_constant(const MessageInfo& message, const VariableInfo& field, std::string text) {
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        text.erase(0, 1);
    }

    int radix = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        radix = 16;
        text.erase(0, 2);
    }

    std::uint64_t value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, radix);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        error(message, field.line, "invalid integer constant");
    }

    const bool is_unsigned = field.type.starts_with("std::uint");
    const unsigned bits = integer_width(field.type);
    std::uint64_t maximum;
    if (is_unsigned) {
        maximum = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
    } else {
        // The negative range includes one more magnitude than the positive range.
        maximum = (std::uint64_t{1} << (bits - 1)) - (negative ? 0 : 1);
    }

    if ((negative && is_unsigned) || value > maximum) {
        error(message, field.line, "integer constant out of range");
    }

    if (negative && bits == 64 && value == (std::uint64_t{1} << 63)) {
        return "(-9223372036854775807LL - 1)";
    }

    return (negative ? "-" : "") + std::to_string(value) + (is_unsigned ? "ULL" : "LL");
}

// Validate constants as data in the declared type's range. Never paste unchecked
// .msg text into a C++ initializer, where it could become arbitrary source code.
std::string validate_constant(const MessageInfo& message, const VariableInfo& field, std::string text) {
    if (!field.builtin || field.dimensions) {
        error(message, field.line, "constants require a scalar or string type");
    }

    if (field.type == "message::MessageString") {
        return string_constant(message, field, text);
    }

    if (field.type == "bool") {
        if (text != "true" && text != "false") {
            error(message, field.line, "bool constant must be true or false");
        }

        return text;
    }

    if (field.type == "float" || field.type == "double") {
        return floating_constant(message, field, std::move(text));
    }

    return integer_constant(message, field, std::move(text));
}

std::string strip_comment(std::string line) {
    bool quoted = false;
    bool escape = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (escape) {
            escape = false;
            continue;
        }

        if (quoted && line[i] == '\\') {
            escape = true;
            continue;
        }

        if (line[i] == '"') {
            quoted = !quoted;
        }

        if (!quoted && (line[i] == '#' || (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/'))) {
            return line.substr(0, i);
        }
    }

    return line;
}

std::string array_type(const VariableInfo& field) {
    auto type = field.builtin ? field.type : "::vinc::" + field.type;
    for (unsigned i = 0; i < field.dimensions; ++i) {
        type = "message::MessageArray<" + type + ">";
    }

    return type;
}

std::string member_type(const VariableInfo& field) {
    if (field.dimensions || !field.builtin || field.type == "message::MessageString") {
        return array_type(field);
    }

    return "message::Scalar<" + field.type + ">";
}

std::string field_type(const VariableInfo& field) {
    if (field.dimensions) {
        auto element = field;
        --element.dimensions;
        return "message::detail::type<" + array_type(element) + ">()";
    }

    if (!field.builtin) {
        return "message::detail::record_type<::vinc::" + field.type + ">()";
    }

    if (field.type == "message::MessageString") {
        return "message::detail::type<std::uint8_t>()";
    }

    return "message::detail::type<" + field.type + ">()";
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

VariableInfo parse_declaration(const MessageInfo& message, std::string line, std::uint32_t line_number,
                               std::set<std::string>& field_names) {
    static const std::regex declaration(
        R"(^([A-Za-z_][A-Za-z0-9_:]*)(\[\])*[ \t]+([A-Za-z_][A-Za-z0-9_]*)(\[\])*[ \t]*(=.*)?$)");
    if (line.size() > max_declaration_bytes) {
        error(message, line_number, "field declaration exceeds 4096 bytes");
    }

    if (line.back() == ';') {
        line = trim(line.substr(0, line.size() - 1));
    }

    std::smatch match;
    if (!std::regex_match(line, match, declaration)) {
        error(message, line_number, "invalid field declaration");
    }

    constexpr std::size_t type_group = 1;
    constexpr std::size_t name_group = 3;
    constexpr std::size_t constant_group = 5;

    VariableInfo field;
    field.line = line_number;
    field.type = match[type_group].str();
    field.name = match[name_group].str();
    if (!valid_identifier(field.name) || field.name == message.name ||
        field.name == message.name + "Data_t" || field.name == message.name + "Data_tRef" ||
        !field_names.insert(field.name).second) {
        error(message, line_number, "invalid, reserved or duplicate field name: " + field.name);
    }

    const auto name_at = static_cast<std::size_t>(match.position(name_group));
    const auto value_at = match[constant_group].matched
                              ? static_cast<std::size_t>(match.position(constant_group))
                              : line.size();
    field.dimensions = static_cast<unsigned>(
        std::count(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(value_at), '['));
    if (field.dimensions > max_array_dimensions) {
        error(message, line_number, "array nesting exceeds 64");
    }

    if (line.substr(0, name_at).find('[') != std::string::npos &&
        line.substr(name_at, value_at - name_at).find('[') != std::string::npos) {
        error(message, line_number, "place array brackets on either the type or the field name");
    }

    const auto canonical = canonical_builtin(field.type);
    field.builtin = !canonical.empty();
    if (field.builtin) {
        field.type = canonical;
    }

    if (match[constant_group].matched) {
        field.constant = validate_constant(message, field, trim(match[constant_group].str().substr(1)));
    }

    return field;
}

void parse_fields(MessageInfo& message) {
    const auto input = read_file(message.path);
    std::istringstream lines(input);
    std::string line;
    std::uint32_t line_number = 0;
    std::set<std::string> field_names;
    while (std::getline(lines, line)) {
        ++line_number;
        line = trim(strip_comment(line));
        if (line.empty()) {
            continue;
        }

        auto field = parse_declaration(message, line, line_number, field_names);
        message.variables.push_back(std::move(field));
        if (message.variables.size() > max_fields_per_message) {
            error(message, line_number, "message exceeds 4096 fields/constants");
        }
    }
}

}

// Sort input paths for reproducible output and reject names that collide on
// case-insensitive filesystems, even when generation runs on a case-sensitive OS.
std::vector<MessageInfo> Ribosome::load() {
    std::vector<fs::path> paths;
    std::set<fs::path> directories;
    for (const auto& input : inputs_) {
        if (!fs::is_directory(input)) {
            throw std::runtime_error("Input path does not exist or is not a directory: " + path_text(input));
        }

        if (!directories.insert(fs::canonical(input)).second) {
            continue;
        }

        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && entry.path().extension() == ".msg") {
                paths.push_back(entry.path());
            }
        }
    }

    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        throw std::runtime_error("No .msg inputs found");
    }

    std::vector<MessageInfo> messages;
    std::set<std::string> file_names{"all_messages", "message_types"};
    for (const auto& path : paths) {
        MessageInfo message;
        message.path = path;
        message.name = path_text(path.stem());
        if (!valid_identifier(message.name) || reserved_message_name(message.name) ||
            !canonical_builtin(message.name).empty()) {
            error(message, 1, "invalid or reserved message name");
        }

        auto lower = message.name;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }

        if (!file_names.insert(lower).second) {
            error(message, 1, "message names collide on case-insensitive filesystems");
        }

        if (fs::file_size(path) > max_definition_bytes) {
            error(message, 1, "message definition exceeds 1 MiB");
        }

        parse_fields(message);
        messages.push_back(std::move(message));
    }

    return messages;
}

// Depth-first dependency resolution rejects cycles before emission. Parent hashes
// include dependent hashes, so changing a nested message also changes its containing schema.
void Ribosome::resolve(std::vector<MessageInfo>& messages) {
    std::map<std::string, std::size_t> index;
    std::set<std::string> symbols;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        for (const auto& name : {message.name, message.name + "Data_t", message.name + "Data_tRef",
                                 "ribosome_schema_" + message.name}) {
            if (!symbols.insert(name).second) {
                error(message, 1, "generated symbol collision: " + name);
            }
        }

        index.emplace(message.name, i);
    }

    std::vector<VisitState> state(messages.size(), VisitState::Unvisited);
    std::vector<unsigned> dependency_depths(messages.size(), 0);
    std::vector<unsigned> initial_depths(messages.size(), 0);
    std::function<void(std::size_t, unsigned)> visit = [&](std::size_t id, unsigned depth) {
        auto& message = messages[id];
        // Cached subtrees retain their longest path, so filename/visit order
        // cannot bypass a parent's depth limit.
        if (state[id] == VisitState::Complete) {
            return;
        }

        if (state[id] == VisitState::Visiting) {
            error(message, 1, "cyclic message dependency");
        }

        if (depth > max_dependency_depth) {
            error(message, 1, "message dependency nesting exceeds 64");
        }

        state[id] = VisitState::Visiting;
        crypto::SHA256 sha;
        // Length-prefix hash inputs to distinguish different field/token boundaries.
        // Whitespace and comments never participate in the canonical schema fingerprint.
        auto token = [&](const std::string& value) {
            sha.update(std::to_string(value.size()) + ":" + value);
        };
        token("vinc-record-wire-v2");
        token(message.name);
        for (const auto& field : message.variables) {
            token(field.type);
            token(field.name);
            token(std::to_string(field.dimensions));
            token(field.constant.value_or("<field>"));
            if (!field.builtin) {
                const auto found = index.find(field.type);
                if (found == index.end()) {
                    error(message, field.line, "Unknown type: " + field.type);
                }

                visit(found->second, depth + 1);
                token(std::to_string(messages[found->second].hash));
                dependency_depths[id] = std::max(dependency_depths[id], dependency_depths[found->second] + 1);
                if (dependency_depths[id] > max_dependency_depth) {
                    error(message, field.line, "message dependency nesting exceeds 64");
                }
                if (!field.dimensions) {
                    initial_depths[id] = std::max(initial_depths[id], initial_depths[found->second] + 1);
                }
            }
            if (!field.constant && (field.builtin || field.dimensions)) {
                initial_depths[id] = std::max(initial_depths[id], 1u);
            }
            // Storage starts the root record at zero and counts each field.
            // Empty arrays stop initial allocation, but still count as a field.
            if (initial_depths[id] > max_initial_schema_depth) {
                error(message, field.line, "initial schema nesting exceeds 64");
            }
        }

        const auto digest = sha.digest();
        for (unsigned i = 0; i < schema_hash_bytes; ++i) {
            message.hash = (message.hash << 8) | digest[i];
        }

        if (!message.hash) {
            error(message, 1, "reserved zero schema hash");
        }

        state[id] = VisitState::Complete;
    };
    std::set<std::uint64_t> hashes;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        visit(i, 0);
        if (!hashes.insert(messages[i].hash).second) {
            error(messages[i], 1, "schema hash collision");
        }
    }
}

namespace {
void emit_dependencies(std::ostream& output, const MessageInfo& message) {
    std::set<std::string> dependencies;
    for (const auto& field : message.variables) {
        if (!field.builtin) {
            dependencies.insert(field.type);
        }
    }

    for (const auto& name : dependencies) {
        output << "#include <vinc_messages/" << name << ".hpp>\n";
    }
}

void emit_schema(std::ostream& output, const MessageInfo& message, const std::string& schema_name) {
    output << "inline const message::detail::Schema& " << schema_name << "() {\n"
           << "    static const message::detail::Schema value{" << message.hash << "ULL, {\n";
    for (const auto& field : message.variables) {
        if (field.constant) {
            continue;
        }

        const bool scalar_field =
            field.builtin && !field.dimensions && field.type != "message::MessageString";
        output << "        {\"" << field.name << "\", " << field_type(field) << ", "
               << (scalar_field ? "true" : "false") << "},\n";
    }

    output << "    }};\n"
           << "    return value;\n"
           << "}\n\n";
}

void emit_data_fields(std::ostream& output, const MessageInfo& message, const std::string& data_name) {
    output << "class " << message.name << ";\n\n";
    output << "template<bool ReadOnly> struct " << data_name << "Ref {\n";
    for (const auto& field : message.variables) {
        if (!field.constant) {
            output << "    message::FieldRef<" << member_type(field) << ", ReadOnly> " << field.name << ";\n";
        }
    }
    output << "    explicit " << data_name << "Ref(message::detail::Access access)";
    std::size_t proxy_index = 0;
    for (const auto& field : message.variables) {
        if (!field.constant) {
            output << (proxy_index == 0 ? "\n        : " : ",\n          ") << field.name
                   << "(message::detail::access_child(access, " << proxy_index << "))";
            ++proxy_index;
        }
    }
    output << " {\n        message::detail::validate_access(access);\n    }\n";
    output << "    " << data_name << "Ref(const " << data_name << "Ref&) = default;\n"
           << "    " << data_name << "Ref(" << data_name << "Ref&&) = default;\n"
           << "    " << data_name << "Ref& operator=(const " << data_name << "Ref&) = delete;\n"
           << "    " << data_name << "Ref& operator=(" << data_name << "Ref&&) = delete;\n"
           << "};\n\n";
    output << "struct " << data_name << " {\n"
           << "private:\n"
           << "    friend class " << message.name << ";\n"
           << "    message::detail::Binding binding_;\n\n"
           << "public:\n";
    for (const auto& field : message.variables) {
        if (!field.constant) {
            output << "    " << member_type(field) << " " << field.name << ";\n";
        }
    }
}

void emit_data_constructors(std::ostream& output, const MessageInfo& message, const std::string& schema_name,
                            const std::string& data_name) {
    output << "\n    explicit " << data_name << "(message::detail::Binding b)\n"
           << "        : binding_(b)";
    std::size_t field_index = 0;
    for (const auto& field : message.variables) {
        if (field.constant) {
            continue;
        }

        output << ",\n          " << field.name << "(message::detail::RecordAccess::bind<"
               << member_type(field) << ">(message::detail::field(b, " << field_index << ")))";
        ++field_index;
    }

    output << " {\n    }\n\n"
           << "    " << data_name << "()\n"
           << "        : " << data_name << "(message::detail::construct_record(" << schema_name
           << "())) {\n    }\n\n"
           << "    " << data_name << "(const " << data_name << "& o)\n"
           << "        : " << data_name << "(message::detail::copy_local(o.binding_)) {\n"
           << "    }\n\n"
           << "    " << data_name << "& operator=(const " << data_name << "&) = delete;\n"
           << "    " << data_name << "& operator=(" << data_name << "&&) = delete;\n"
           << "    [[nodiscard]] ::vinc::Result<void> assign(const " << data_name << "& other) {\n"
           << "        (void)message::detail::node_epoch(binding_.state, binding_.handle);\n"
           << "        (void)message::detail::node_epoch(other.binding_.state, other.binding_.handle);\n"
           << "        return message::detail::replace(binding_, other.binding_);\n    }\n"
           << "};\n\n";
}

void emit_message_metadata(std::ostream& output, const MessageInfo& message, const std::string& schema_name) {
    output << "class " << message.name << " : public message::GeneratedMessage<" << message.name << "> {\n"
           << "    using ribosome_Base = message::GeneratedMessage<" << message.name << ">;\n"
           << "    friend class message::detail::RecordAccess;\n\n"
           << "public:\n"
           << "    template<bool ReadOnly> using DataRef = " << message.name << "Data_tRef<ReadOnly>;\n"
           << "    static constexpr std::string_view message_name = \"" << message.name << "\";\n\n"
           << "    static const message::detail::Schema& schema() {\n"
           << "        return " << schema_name << "();\n"
           << "    }\n\n";
    output << "    // Compile-time field visits support language bindings without parsing schemas at runtime.\n"
           << "    template<class Visitor> static void ribosome_visit_fields([[maybe_unused]] Visitor&& visitor) {\n";
    std::size_t index = 0;
    for (const auto& field : message.variables) {
        if (!field.constant) {
            output << "        visitor.template field<" << member_type(field) << ">(\""
                   << field.name << "\", " << index << ");\n";
            ++index;
        }
    }
    output << "    }\n\n";
    output << "    template<class Visitor> static void ribosome_visit_constants([[maybe_unused]] Visitor&& visitor) {\n";
    for (const auto& field : message.variables) {
        if (field.constant) {
            output << "        visitor(\"" << field.name << "\", " << field.name << ");\n";
        }
    }
    output << "    }\n\n";
    for (const auto& field : message.variables) {
        if (!field.constant) {
            continue;
        }

        const auto constant_type = field.type == "message::MessageString" ? "std::string_view" : field.type;
        output << "    static constexpr " << constant_type << " " << field.name << " = " << *field.constant
               << ";\n";
    }
}

void emit_message_constructors(std::ostream& output, const MessageInfo& message,
                               const std::string& data_name) {
    const auto& name = message.name;
    output << "\n    " << name << "()\n"
           << "        : " << name << "(message::detail::BindTag{},\n"
           << "              message::detail::construct_record(schema())) {\n"
           << "    }\n\n"
           << "    // Copies detach their payload while retaining message metadata.\n"
           << "    " << name << "(const " << name << "& o)\n"
           << "        : " << name << "(message::detail::BindTag{}, ribosome_Base::copy_binding(o)) {\n"
           << "        BaseMessage::operator=(o);\n"
           << "    }\n\n"
           << "    " << name << "(" << name << "&& o)\n"
           << "        : " << name << "(message::detail::BindTag{}, ribosome_Base::move_binding(o)) {\n"
           << "        BaseMessage::operator=(o);\n"
           << "        // A transferred binding leaves no facade in the source.\n"
           << "        if (!o.binding_.state) {\n"
           << "            o.data_.reset();\n"
           << "        }\n"
           << "    }\n\n"
           << "    " << name << "& operator=(const " << name << "&) = delete;\n"
           << "    " << name << "& operator=(" << name << "&&) = delete;\n\n"
           << "    explicit " << name << "(const " << data_name << "& d)\n"
           << "        : " << name
           << "(message::detail::BindTag{}, message::detail::copy_local(d.binding_)) {}\n\n";
}

void emit_data_accessors(std::ostream& output, const std::string& data_name) {
    output << "    " << data_name << "& data() {\n"
           << "        (void)message::detail::epoch(binding_.state);\n"
           << "        return *data_;\n"
           << "    }\n\n"
           << "    const " << data_name << "& data() const {\n"
           << "        (void)message::detail::epoch(binding_.state);\n"
           << "        return *data_;\n"
           << "    }\n\n";
}

void emit_text_representation(std::ostream& output, const MessageInfo& message) {
    output << "    template<class Fields> static std::string to_string(const Fields& d) {\n"
           << "        (void)d;\n"
           << "        std::ostringstream out;\n"
           << "        out << std::boolalpha << \"" << message.name << " {\";\n";
    bool first_field = true;
    for (const auto& field : message.variables) {
        if (field.constant) {
            continue;
        }

        output << "        out << \"" << (first_field ? "" : ", ") << field.name << ": \";\n"
               << "        message::detail::print(out, d." << field.name << ");\n";
        first_field = false;
    }

    output << "        out << '}';\n"
           << "        return out.str();\n"
           << "    }\n\n"
           << "    std::string to_string() const override {\n"
           << "        return to_string(data());\n"
           << "    }\n\n";
}

void emit_bound_constructor(std::ostream& output, const MessageInfo& message, const std::string& data_name) {
    output << "private:\n"
           << "    " << message.name << "(message::detail::BindTag, message::detail::Binding b)\n"
           << "        : ribosome_Base(std::move(b)), data_(std::make_unique<" << data_name << ">(binding_)) {\n"
           << "    }\n\n"
           << "    std::unique_ptr<" << data_name << "> data_;\n"
           << "};\n";
}
}

// Emit a local class facade and bound Data_t fields from the validated schema.
// Field constructors bind into one arena; no C++ object layout is used as the wire format.
std::string Ribosome::generate(const MessageInfo& message, const std::string& source) {
    std::ostringstream output;
    std::ostringstream includes;
    const auto schema_name = "ribosome_schema_" + message.name;
    const auto data_name = message.name + "Data_t";

    emit_dependencies(includes, message);
    emit_schema(output, message, schema_name);
    emit_data_fields(output, message, data_name);
    emit_data_constructors(output, message, schema_name, data_name);
    emit_message_metadata(output, message, schema_name);
    emit_message_constructors(output, message, data_name);
    emit_data_accessors(output, data_name);
    emit_text_representation(output, message);
    emit_bound_constructor(output, message, data_name);

    auto text = source;
    replace_all(text, "${DO_INCLUDE_DEPENDENT_MESSAGES}", includes.str());
    replace_all(text, "${DO_MESSAGE_CLASS}", output.str());
    return text;
}

namespace {
struct OutputPaths {
    fs::path output;
    fs::path source_template;
};

// A stage is owned only after create_directory succeeds. Never remove another run's stage.
struct StagingDirectory {
    fs::path path;
    bool owned = false;

    ~StagingDirectory() {
        if (owned) {
            std::error_code cleanup_error;
            fs::remove_all(path, cleanup_error);
        }
    }
};

OutputPaths validate_output_paths(const fs::path& requested_output, const std::vector<fs::path>& inputs,
                                  const fs::path& requested_template) {
    if (requested_output.empty() || requested_output.filename().empty() ||
        requested_output.filename() == "." || requested_output.filename() == "..") {
        throw std::runtime_error("Invalid output directory");
    }

    const auto output = fs::absolute(requested_output).lexically_normal();
    if (fs::is_symlink(fs::symlink_status(output))) {
        throw std::runtime_error("Output directory must not be a symlink");
    }

    const auto path = requested_template.empty() ? fs::path(kDefaultTemplatePath) : requested_template;
    const auto resolved_output = fs::weakly_canonical(output);
    auto protected_paths = inputs;
    protected_paths.push_back(path);
    for (const auto& protected_path : protected_paths) {
        const auto resolved = fs::weakly_canonical(fs::absolute(protected_path));
        const auto relative = resolved.lexically_relative(resolved_output);
        if (resolved == resolved_output || (!relative.empty() && *relative.begin() != "..")) {
            throw std::runtime_error("Output directory contains an input or template path");
        }
    }

    return {output, path};
}

// On failure, the caller's guard removes only the acquired staging directory.
// Old output is intentionally removed before reading or validating the template.
void prepare_staging(StagingDirectory& stage, const fs::path& output) {
    fs::create_directories(output.parent_path());
    auto stage_name = output.filename();
    stage_name += ".ribosome-stage";
    stage.path = output.parent_path() / stage_name;
    if (!fs::create_directory(stage.path)) {
        throw std::runtime_error("Staging directory already exists");
    }

    stage.owned = true;
    fs::remove_all(output);
}

std::string load_template(const fs::path& path) {
    if (!fs::exists(path)) {
        throw std::runtime_error("Failed to open message template: " + path_text(path));
    }

    auto source = read_file(path);
    if (trim(source).empty()) {
        throw std::runtime_error("Message template is empty");
    }

    std::set<std::string> placeholders;
    std::size_t position = 0;
    while ((position = source.find("${", position)) != std::string::npos) {
        const auto end = source.find('}', position + 2);
        if (end == std::string::npos) {
            throw std::runtime_error("Unterminated template placeholder");
        }

        const auto name = source.substr(position + 2, end - position - 2);
        if (name != "DO_MESSAGE_CLASS" && name != "DO_INCLUDE_DEPENDENT_MESSAGES") {
            throw std::runtime_error("Unknown template placeholder: " + name);
        }

        if (!placeholders.insert(name).second) {
            throw std::runtime_error("Duplicate template placeholder: " + name);
        }
        position = end + 1;
    }
    if (placeholders.size() != 2) {
        throw std::runtime_error("Missing required template placeholder");
    }

    return source;
}

void publish_staging(StagingDirectory& stage, const fs::path& output) {
    fs::rename(stage.path, output);
    stage.owned = false;
}
}

// Fail closed: remove the old output and generate the complete new set in staging.
// An error must not leave stale headers that make a later build appear successful.
std::int8_t Ribosome::make_ligand() {
    StagingDirectory stage;
    try {
        const auto paths = validate_output_paths(output_, inputs_, template_);
        prepare_staging(stage, paths.output);

        auto source = load_template(paths.source_template);
        auto messages = load();
        resolve(messages);

        std::ostringstream umbrella;
        umbrella << "#pragma once\n";
        for (const auto& message : messages) {
            write_file(stage.path / (message.name + ".hpp"), generate(message, source));
            umbrella << "#include <vinc_messages/" << message.name << ".hpp>\n";
        }

        write_file(stage.path / "all_messages.hpp", umbrella.str());
        std::ostringstream catalog;
        catalog << "#pragma once\n#include <vinc_messages/all_messages.hpp>\n"
                   "namespace vinc::message {\n"
                   "template<class Visitor> void visit_types(Visitor&& visitor) {\n";
        for (const auto& message : messages) {
            catalog << "    visitor.template operator()<::vinc::" << message.name << ">();\n";
        }
        catalog << "}\n}\n";
        write_file(stage.path / "message_types.hpp", catalog.str());
        publish_staging(stage, paths.output);

        std::cout << "Generated " << messages.size() << " message classes at " << path_text(paths.output) << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAILED] " << exception.what() << '\n';
        return -1;
    }
}
}

namespace {
template<class Character>
int run_cli(int argc, Character** argv) {
    vinc::Ribosome generator;
#ifndef VINC_RIBOSOME_INSTALLED_TOOL
    generator.set_text_message_path(std::filesystem::path(vinc::kDefaultInputPath));
    generator.set_cpp_message_path(std::filesystem::path(vinc::kDefaultOutputPath));
#endif
    bool explicit_input = false;
#ifdef VINC_RIBOSOME_INSTALLED_TOOL
    bool explicit_output = false;
    bool explicit_template = false;
#endif
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            std::cerr << "Expected --input/--output/--template PATH\n";
            return 1;
        }

        const std::filesystem::path option = argv[i];
        if (option == std::filesystem::path("--input")) {
            if (!explicit_input) {
                generator.set_text_message_path(argv[i + 1]);
                explicit_input = true;
            } else {
                generator.add_text_message_path(argv[i + 1]);
            }
        } else if (option == std::filesystem::path("--output")) {
            generator.set_cpp_message_path(argv[i + 1]);
#ifdef VINC_RIBOSOME_INSTALLED_TOOL
            explicit_output = true;
#endif
        } else if (option == std::filesystem::path("--template")) {
            generator.set_template_path(argv[i + 1]);
#ifdef VINC_RIBOSOME_INSTALLED_TOOL
            explicit_template = true;
#endif
        } else {
            const auto utf8 = option.u8string();
            std::cerr << "Unknown option: " << std::string(utf8.begin(), utf8.end()) << '\n';
            return 1;
        }
    }

#ifdef VINC_RIBOSOME_INSTALLED_TOOL
    // Installed tools must never fall back to paths on the SDK producer's disk.
    if (!explicit_input || !explicit_output || !explicit_template) {
        std::cerr << "Installed Ribosome requires explicit --input, --output and --template paths\n";
        return 1;
    }
#endif
    return generator.make_ligand();
}

template<class Character>
int guarded_cli(int argc, Character** argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "[FAILED] " << exception.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
// The Windows CRT supplies the original UTF-16 arguments, without an ANSI round-trip.
int wmain(int argc, wchar_t** argv) {
    return guarded_cli(argc, argv);
}
#else
int main(int argc, char** argv) {
    return guarded_cli(argc, argv);
}
#endif
