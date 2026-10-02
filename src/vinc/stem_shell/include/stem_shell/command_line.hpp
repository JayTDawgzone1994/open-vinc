#pragma once

// Interactive Shell parsing and line editing only; Tcl scripts use Tcl's own parser.
// Arguments stay as a vector after parsing so quoting is not interpreted twice.

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace vinc::shell {
inline constexpr std::size_t max_line_bytes = 4096;
struct ParsedLine {
    std::vector<std::string> arguments;
    std::size_t last_start = 0;
    bool trailing_space = false;
};
// Completion may parse an unfinished quote. Execution requires complete quoting.
std::expected<ParsedLine, std::string> parse_line(std::string_view, bool completion = false);
std::string quote_argument(std::string_view);
// End-of-line editor. Stores UTF-8; backspace removes one complete code point.
class LineEditor {
    std::string line_, draft_;
    std::vector<std::string> history_;
    std::size_t position_ = 0;
public:
    const std::string& line() const noexcept { return line_; }
    bool append(std::string_view);
    bool replace(std::string);
    void backspace();
    void older();
    void newer();
    std::string submit();
    // Conservative display bound: non-ASCII code points reserve two columns.
    std::string visible_tail(std::size_t columns) const;
};
}
