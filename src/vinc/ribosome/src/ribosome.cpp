// Ribosome pipeline: validate .msg syntax, canonicalize types, resolve dependencies,
// fingerprint schemas, and publish a complete set of generated headers from a staging directory.

#include <ribosome/ribosome.hpp>
#include <ribosome/sha256.hpp>
#include <generated/ribosome_config.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vinc {
namespace {
namespace fs = std::filesystem;
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Failed to open file: " + path.string());
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (file.bad()) throw std::runtime_error("Failed to read file: " + path.string());
    return text;
}
void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text; file.close();
    if (!file) throw std::runtime_error("Failed to write generated header: " + path.string());
}
std::string trim(std::string text) {
    const auto start = text.find_first_not_of(" \r\n\t");
    if (start == std::string::npos) return {};
    return text.substr(start, text.find_last_not_of(" \r\n\t") - start + 1);
}
bool identifier(const std::string& name) {
    static const std::regex pattern("[A-Za-z][A-Za-z0-9_]*");
    static const std::set<std::string> reserved{
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break","case","catch","char","char8_t","char16_t","char32_t",
        "class","compl","concept","const","consteval","constexpr","constinit","const_cast","continue","co_await","co_return","co_yield",
        "decltype","default","delete","do","double","dynamic_cast","else","enum","explicit","export","extern","false","float","for","friend",
        "goto","if","inline","int","long","mutable","namespace","new","noexcept","not","not_eq","nullptr","operator","or","or_eq",
        "private","protected","public","register","reinterpret_cast","requires","return","short","signed","sizeof","static","static_assert",
        "static_cast","struct","switch","template","this","thread_local","throw","true","try","typedef","typeid","typename","union","unsigned",
        "using","virtual","void","volatile","wchar_t","while","xor","xor_eq","all_messages",
        "schema","data","snapshot","usage","compact","serialize","deserialize","assign","from_bytes","to_string","type_hash","msg_name_str",
        "get_type_hash_static","message_name","binding","binding_","data_","topic_name","publisher_name","get_message_timestamp","get_topic_count","publisher_instance",
        "message","BaseMessage","MessageArray","MessageArrayView","Ribosome"
    };
    return std::regex_match(name, pattern) && name.find("__") == std::string::npos && !name.starts_with("ribosome_") && !reserved.contains(name);
}
// Resolve source aliases to explicit widths before hashing or emitting C++.
// For example, the schema meaning of long is int64 even on an OS where native long is 32-bit.
std::string builtin(std::string type) {
    if (type.starts_with("std::")) type.erase(0, 5);
    static const std::map<std::string, std::string> aliases{
        {"int","std::int32_t"},{"short","std::int16_t"},{"long","std::int64_t"},{"char","std::int8_t"},{"size_t","std::uint64_t"},
        {"int8","std::int8_t"},{"uint8","std::uint8_t"},{"int16","std::int16_t"},{"uint16","std::uint16_t"},
        {"int32","std::int32_t"},{"uint32","std::uint32_t"},{"int64","std::int64_t"},{"uint64","std::uint64_t"},
        {"int8_t","std::int8_t"},{"uint8_t","std::uint8_t"},{"int16_t","std::int16_t"},{"uint16_t","std::uint16_t"},
        {"int32_t","std::int32_t"},{"uint32_t","std::uint32_t"},{"int64_t","std::int64_t"},{"uint64_t","std::uint64_t"},
        {"float","float"},{"double","double"},{"bool","bool"},{"string","message::MessageString"}
    };
    const auto found = aliases.find(type); return found == aliases.end() ? "" : found->second;
}
[[noreturn]] void error(const MessageInfo& m, std::uint32_t line, const std::string& reason) {
    throw std::runtime_error(m.path.string() + ":" + std::to_string(line) + ": error: " + reason);
}
// Validate constants as data in the declared type's range. Never paste unchecked
// .msg text into a C++ initializer, where it could become arbitrary source code.
std::string constant(const MessageInfo& m, const VariableInfo& f, std::string text) {
    if (!f.builtin || f.dimensions) error(m, f.line, "constants require a scalar or string type");
    if (f.type == "message::MessageString") {
        static const std::regex literal(R"rx("([^"\\\r\n]|\\[nrt0"\\])*")rx");
        if (!std::regex_match(text, literal)) error(m, f.line, "invalid string constant");
        return text;
    }
    if (f.type == "bool") {
        if (text != "true" && text != "false") error(m, f.line, "bool constant must be true or false");
        return text;
    }
    if (f.type == "float" || f.type == "double") {
        if (text == "nan") return "std::numeric_limits<" + f.type + ">::quiet_NaN()";
        if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
        double value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value) ||
            (f.type == "float" && !std::isfinite(static_cast<float>(value)))) error(m, f.line, "invalid or out-of-range floating constant");
        return "static_cast<" + f.type + ">(" + text + ")";
    }
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) { negative = text[0] == '-'; text.erase(0, 1); }
    int radix = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) { radix = 16; text.erase(0, 2); }
    std::uint64_t value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, radix);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) error(m, f.line, "invalid integer constant");
    const bool unsign = f.type.starts_with("std::uint");
    const unsigned bits = f.type.find("8_t") != std::string::npos ? 8 : f.type.find("16_t") != std::string::npos ? 16 : f.type.find("32_t") != std::string::npos ? 32 : 64;
    const auto maximum = unsign ? (bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1) : (std::uint64_t{1} << (bits - 1)) - (negative ? 0 : 1);
    if ((negative && unsign) || value > maximum) error(m, f.line, "integer constant out of range");
    if (negative && bits == 64 && value == (std::uint64_t{1} << 63)) return "(-9223372036854775807LL - 1)";
    return (negative ? "-" : "") + std::to_string(value) + (unsign ? "ULL" : "LL");
}
std::string strip_comment(std::string line) {
    bool quoted = false, escape = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (escape) { escape = false; continue; }
        if (quoted && line[i] == '\\') { escape = true; continue; }
        if (line[i] == '"') quoted = !quoted;
        if (!quoted && (line[i] == '#' || (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/'))) return line.substr(0, i);
    }
    return line;
}
std::string array_type(const VariableInfo& f) {
    auto type = f.type;
    for (unsigned i = 0; i < f.dimensions; ++i) type = "message::MessageArray<" + type + ">";
    return type;
}
std::string member_type(const VariableInfo& f) {
    if (f.dimensions || !f.builtin || f.type == "message::MessageString") return array_type(f);
    return "message::Scalar<" + f.type + ">";
}
std::string field_type(const VariableInfo& f) {
    if (f.dimensions) {
        auto element = f; --element.dimensions;
        return "message::detail::type<" + array_type(element) + ">()";
    }
    if (!f.builtin) return "message::detail::record_type<" + f.type + ">()";
    if (f.type == "message::MessageString") return "message::detail::type<std::uint8_t>()";
    return "message::detail::type<" + f.type + ">()";
}
void replace_all(std::string& text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) { text.replace(pos, from.size(), to); pos += to.size(); }
}
}

// Sort input paths for reproducible output and reject names that collide on
// case-insensitive filesystems, even when generation runs on a case-sensitive OS.
std::vector<MessageInfo> Ribosome::load() {
    if (!fs::is_directory(input_)) throw std::runtime_error("Input path does not exist or is not a directory: " + input_.string());
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(input_)) if (entry.is_regular_file() && entry.path().extension() == ".msg") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) throw std::runtime_error("No .msg inputs found");
    std::vector<MessageInfo> messages;
    std::set<std::string> file_names;
    static const std::regex declaration(R"(^([A-Za-z_][A-Za-z0-9_:]*)(\[\])*[ \t]+([A-Za-z_][A-Za-z0-9_]*)(\[\])*[ \t]*(=.*)?$)");
    for (const auto& path : paths) {
        MessageInfo m; m.path = path; m.name = path.stem().string();
        if (!identifier(m.name) || !builtin(m.name).empty()) error(m, 1, "invalid or reserved message name");
        auto lower = m.name; for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (!file_names.insert(lower).second) error(m, 1, "message names collide on case-insensitive filesystems");
        if (fs::file_size(path) > 1024 * 1024) error(m, 1, "message definition exceeds 1 MiB");
        const auto input = read_file(path);
        std::istringstream lines(input); std::string line; std::uint32_t number = 0;
        std::set<std::string> names;
        while (std::getline(lines, line)) {
            ++number; line = trim(strip_comment(line)); if (line.empty()) continue;
            if (line.size() > 4096) error(m, number, "field declaration exceeds 4096 bytes");
            if (line.back() == ';') line = trim(line.substr(0, line.size() - 1));
            std::smatch match;
            if (!std::regex_match(line, match, declaration)) error(m, number, "invalid field declaration");
            VariableInfo f; f.line = number; f.type = match[1].str(); f.name = match[3].str();
            if (!identifier(f.name) || f.name == m.name || f.name == m.name + "Data_t" || !names.insert(f.name).second) error(m, number, "invalid, reserved or duplicate field name: " + f.name);
            const auto name_at = static_cast<std::size_t>(match.position(3));
            const auto value_at = match[5].matched ? static_cast<std::size_t>(match.position(5)) : line.size();
            f.dimensions = static_cast<unsigned>(std::count(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(value_at), '['));
            if (f.dimensions > 64) error(m, number, "array nesting exceeds 64");
            if (line.substr(0, name_at).find('[') != std::string::npos && line.substr(name_at, value_at - name_at).find('[') != std::string::npos)
                error(m, number, "place array brackets on either the type or the field name");
            const auto canonical = builtin(f.type);
            f.builtin = !canonical.empty(); if (f.builtin) f.type = canonical;
            if (match[5].matched) f.constant = constant(m, f, trim(match[5].str().substr(1)));
            m.variables.push_back(std::move(f));
            if (m.variables.size() > 4096) error(m, number, "message exceeds 4096 fields/constants");
        }
        messages.push_back(std::move(m));
    }
    return messages;
}

// Depth-first dependency resolution rejects cycles before emission. Parent hashes
// include dependent hashes, so changing a nested message also changes its containing schema.
void Ribosome::resolve(std::vector<MessageInfo>& messages) {
    std::map<std::string, std::size_t> index;
    std::set<std::string> symbols;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& m = messages[i];
        for (const auto& name : {m.name, m.name + "Data_t", "ribosome_schema_" + m.name})
            if (!symbols.insert(name).second) error(m, 1, "generated symbol collision: " + name);
        index.emplace(m.name, i);
    }
    std::vector<unsigned> state(messages.size());
    std::function<void(std::size_t, unsigned)> visit = [&](std::size_t id, unsigned depth) {
        auto& m = messages[id];
        if (state[id] == 2) return;
        if (state[id] == 1) error(m, 1, "cyclic message dependency");
        if (depth > 64) error(m, 1, "message dependency nesting exceeds 64");
        state[id] = 1;
        crypto::SHA256 sha;
        // Length-prefix hash inputs to distinguish different field/token boundaries.
        // Whitespace and comments never participate in the canonical schema fingerprint.
        auto token = [&](const std::string& s) { sha.update(std::to_string(s.size()) + ":" + s); };
        token("vinc-record-wire-v2"); token(m.name);
        for (const auto& f : m.variables) {
            token(f.type); token(f.name); token(std::to_string(f.dimensions)); token(f.constant.value_or("<field>"));
            if (!f.builtin) {
                const auto found = index.find(f.type);
                if (found == index.end()) error(m, f.line, "Unknown type: " + f.type);
                visit(found->second, depth + 1); token(std::to_string(messages[found->second].hash));
            }
        }
        const auto digest = sha.digest();
        for (unsigned i = 0; i < 8; ++i) m.hash = (m.hash << 8) | digest[i];
        if (!m.hash) error(m, 1, "reserved zero schema hash");
        state[id] = 2;
    };
    std::set<std::uint64_t> hashes;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        visit(i, 0);
        if (!hashes.insert(messages[i].hash).second) error(messages[i], 1, "schema hash collision");
    }
}

// Emit a local class facade and bound Data_t fields from the validated schema.
// Field constructors bind into one arena; no C++ object layout is used as the wire format.
std::string Ribosome::generate(const MessageInfo& m, const std::string& source) {
    std::ostringstream out, includes;
    std::set<std::string> dependencies;
    for (const auto& f : m.variables) if (!f.builtin) dependencies.insert(f.type);
    for (const auto& name : dependencies) includes << "#include <vinc_messages/" << name << ".hpp>\n";
    const auto schema_name = "ribosome_schema_" + m.name;
    const auto data = m.name + "Data_t";
    out << "inline const message::detail::Schema& " << schema_name << "() {\n"
        << "    static const message::detail::Schema value{" << m.hash << "ULL, {\n";
    for (const auto& f : m.variables) if (!f.constant)
        out << "        {\"" << f.name << "\", " << field_type(f) << ", " << (f.builtin && !f.dimensions && f.type != "message::MessageString" ? "true" : "false") << "},\n";
    out << "    }};\n    return value;\n}\n\n"
        << "class " << m.name << ";\nstruct " << data << " {\nprivate:\n"
        << "    friend class " << m.name << ";\n    message::detail::Binding binding_;\npublic:\n";
    for (const auto& f : m.variables) if (!f.constant) out << "    " << member_type(f) << " " << f.name << ";\n";
    out << "    explicit " << data << "(message::detail::Binding b) : binding_(b)";
    std::size_t index = 0;
    for (const auto& f : m.variables) if (!f.constant)
        out << ",\n        " << f.name << "(message::detail::RecordAccess::bind<" << member_type(f) << ">(message::detail::field(b, " << index++ << ")))";
    out << " {}\n    " << data << "() : " << data << "(message::detail::checked(message::detail::create_record(" << schema_name << "()))) {}\n"
        << "    " << data << "(const " << data << "& o) : " << data << "(message::detail::checked(message::detail::clone(o.binding_))) {}\n"
        << "    " << data << "& operator=(const " << data << "& o) { message::detail::checked(message::detail::replace(binding_, o.binding_)); return *this; }\n"
        << "};\n\nclass " << m.name << " : public message::GeneratedMessage<" << m.name << "> {\n"
        << "    using Base = message::GeneratedMessage<" << m.name << ">;\n"
        << "    friend class message::detail::RecordAccess;\npublic:\n"
        << "    static constexpr std::string_view message_name = \"" << m.name << "\";\n"
        << "    static const message::detail::Schema& schema() { return " << schema_name << "(); }\n";
    for (const auto& f : m.variables) if (f.constant)
        out << "    static constexpr " << (f.type == "message::MessageString" ? "std::string_view" : f.type) << " " << f.name << " = " << *f.constant << ";\n";
    out << "    " << m.name << "() : " << m.name << "(message::detail::BindTag{}, message::detail::checked(message::detail::create_record(schema()))) {}\n"
        << "    " << m.name << "(const " << m.name << "& o) : " << m.name << "(message::detail::BindTag{}, Base::copy_binding(o)) { BaseMessage::operator=(o); }\n"
        << "    " << m.name << "(" << m.name << "&& o) : " << m.name << "(message::detail::BindTag{}, Base::move_binding(o)) { BaseMessage::operator=(o); if (!o.binding_.state) o.data_.reset(); }\n"
        << "    " << m.name << "& operator=(const " << m.name << "& o) { message::detail::checked(this->assign(o)); return *this; }\n"
        << "    " << m.name << "& operator=(" << m.name << "&& o) { return *this = static_cast<const " << m.name << "&>(o); }\n"
        << "    explicit " << m.name << "(const " << data << "& d) : " << m.name << "() { message::detail::checked(message::detail::replace(binding_, d.binding_)); }\n"
        << "    " << data << "& data() { (void)message::detail::epoch(binding_.state); return *data_; }\n"
        << "    const " << data << "& data() const { (void)message::detail::epoch(binding_.state); return *data_; }\n"
        << "    static std::string to_string(const " << data << "& d) {\n        (void)d; std::ostringstream out; out << std::boolalpha << \"" << m.name << " {\";\n";
    bool first = true;
    for (const auto& f : m.variables) if (!f.constant) {
        out << "        out << \"" << (first ? "" : ", ") << f.name << ": \"; message::detail::print(out, d." << f.name << ");\n"; first = false;
    }
    out << "        out << '}'; return out.str();\n    }\n    std::string to_string() const override { return to_string(data()); }\n"
        << "private:\n    " << m.name << "(message::detail::BindTag, message::detail::Binding b) : Base(std::move(b)), data_(std::make_unique<" << data << ">(binding_)) {}\n"
        << "    std::unique_ptr<" << data << "> data_;\n};\n";
    auto text = source;
    replace_all(text, "${DO_INCLUDE_DEPENDENT_MESSAGES}", includes.str());
    replace_all(text, "${DO_MESSAGE_CLASS}", out.str());
    if (text.find("${") != std::string::npos) throw std::runtime_error("Unknown template placeholder");
    return text;
}

// Fail closed: remove the old output and generate the complete new set in staging.
// An error must not leave stale headers that make a later build appear successful.
std::int8_t Ribosome::make_ligand() {
    struct Staging {
        fs::path path; bool owned = false;
        ~Staging() { if (owned) { std::error_code ec; fs::remove_all(path, ec); } }
    } stage;
    try {
        if (output_.empty() || output_.filename().empty() || output_.filename() == "." || output_.filename() == "..") throw std::runtime_error("Invalid output directory");
        const auto output = fs::absolute(output_).lexically_normal();
        if (fs::is_symlink(fs::symlink_status(output))) throw std::runtime_error("Output directory must not be a symlink");
        const auto path = template_.empty() ? fs::path(kDefaultTemplatePath) : template_;
        const auto resolved_output = fs::weakly_canonical(output);
        for (const auto& protected_path : {input_, path}) {
            const auto resolved = fs::weakly_canonical(fs::absolute(protected_path));
            const auto relative = resolved.lexically_relative(resolved_output);
            if (resolved == resolved_output || (!relative.empty() && *relative.begin() != ".."))
                throw std::runtime_error("Output directory contains an input or template path");
        }
        fs::create_directories(output.parent_path());
        stage.path = output.parent_path() / (output.filename().string() + ".ribosome-stage");
        if (!fs::create_directory(stage.path)) throw std::runtime_error("Staging directory already exists");
        stage.owned = true;
        fs::remove_all(output);
        if (!fs::exists(path)) throw std::runtime_error("Failed to open message template: " + path.string());
        auto source = read_file(path);
        if (trim(source).empty()) throw std::runtime_error("Message template is empty");
        if (source.find("${DO_MESSAGE_CLASS}") == std::string::npos || source.find("${DO_INCLUDE_DEPENDENT_MESSAGES}") == std::string::npos) throw std::runtime_error("Missing required template placeholder");
        auto messages = load(); resolve(messages);
        std::ostringstream umbrella; umbrella << "#pragma once\n";
        for (const auto& m : messages) {
            write_file(stage.path / (m.name + ".hpp"), generate(m, source));
            umbrella << "#include <vinc_messages/" << m.name << ".hpp>\n";
        }
        write_file(stage.path / "all_messages.hpp", umbrella.str());
        fs::rename(stage.path, output); stage.owned = false;
        std::cout << "Generated " << messages.size() << " message classes at " << output << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "[FAILED] " << e.what() << '\n'; return -1; }
}
}

int main(int argc, char** argv) {
    vinc::Ribosome generator;
    generator.set_text_message_path(vinc::kDefaultInputPath);
    generator.set_cpp_message_path(vinc::kDefaultOutputPath);
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) { std::cerr << "Expected --input/--output/--template PATH\n"; return 1; }
        const std::string option = argv[i];
        if (option == "--input") generator.set_text_message_path(argv[i + 1]);
        else if (option == "--output") generator.set_cpp_message_path(argv[i + 1]);
        else if (option == "--template") generator.set_template_path(argv[i + 1]);
        else { std::cerr << "Unknown option: " << option << '\n'; return 1; }
    }
    return generator.make_ligand();
}
