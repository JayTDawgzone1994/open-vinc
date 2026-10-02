// Parse the small interactive command syntax and maintain bounded UTF-8 line history.
// This is not a system shell: there is no variable expansion, pipeline or script execution here.

#include <stem_shell/command_line.hpp>

namespace vinc::shell {
namespace {
bool space(char c) { return c == ' ' || c == '\t'; }
std::size_t previous(std::string_view s, std::size_t end) {
    if (!end) return 0;
    --end;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
    return end;
}
}
std::expected<ParsedLine, std::string> parse_line(std::string_view input, bool completion) {
    if (input.size() > max_line_bytes) return std::unexpected("command exceeds 4096 bytes");
    ParsedLine parsed;
    char quote = 0;
    bool token = false;
    std::string word;
    auto finish = [&] {
        if (token) { parsed.arguments.push_back(std::move(word)); word.clear(); token = false; }
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return std::unexpected("control character in command");
        if (!quote && space(c)) { finish(); parsed.trailing_space = true; continue; }
        parsed.trailing_space = false;
        if (!token) { token = true; parsed.last_start = i; }
        if (quote == '\'') {
            if (c == '\'') quote = 0; else word += c;
        } else if (c == '\\') {
            if (i + 1 < input.size() && (input[i + 1] == '\\' || input[i + 1] == '"' || (!quote && (input[i + 1] == '\'' || space(input[i + 1])))))
                word += input[++i];
            else word += c; // Preserve ordinary Windows path separators, including a final one.
        } else if (quote == '"') {
            if (c == '"') quote = 0; else word += c;
        } else if (c == '\'' || c == '"') quote = c;
        else word += c;
    }
    if (quote && !completion) return std::unexpected("unterminated quote");
    finish();
    if (parsed.arguments.size() > 256) return std::unexpected("command exceeds 256 arguments");
    if (parsed.trailing_space) parsed.last_start = input.size();
    return parsed;
}
std::string quote_argument(std::string_view word) {
    if (!word.empty() && word.find_first_of(" \t\\\"'") == std::string_view::npos) return std::string(word);
    std::string out = "\"";
    for (const auto c : word) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out + '"';
}
bool LineEditor::append(std::string_view text) {
    if (text.size() > max_line_bytes - line_.size()) return false;
    line_ += text; return true;
}
bool LineEditor::replace(std::string value) {
    if (value.size() > max_line_bytes) return false;
    line_ = std::move(value); return true;
}
void LineEditor::backspace() { line_.resize(previous(line_, line_.size())); }
void LineEditor::older() {
    if (history_.empty() || !position_) return;
    if (position_ == history_.size()) draft_ = line_;
    line_ = history_[--position_];
}
void LineEditor::newer() {
    if (position_ >= history_.size()) return;
    ++position_; line_ = position_ == history_.size() ? draft_ : history_[position_];
}
std::string LineEditor::submit() {
    auto result = std::move(line_); line_.clear();
    if (!result.empty() && (history_.empty() || history_.back() != result)) {
        if (history_.size() == 128) history_.erase(history_.begin());
        history_.push_back(result);
    }
    position_ = history_.size(); draft_.clear(); return result;
}
std::string LineEditor::visible_tail(std::size_t columns) const {
    auto begin = line_.size(); std::size_t used = 0;
    while (begin) {
        const auto next = previous(line_, begin);
        const std::size_t width = static_cast<unsigned char>(line_[next]) < 128 ? 1 : 2;
        if (used + width > columns) break;
        begin = next; used += width;
    }
    return line_.substr(begin);
}
}
