// Parse the small interactive command syntax and maintain bounded UTF-8 line history.
// This is not a system shell: there is no variable expansion, pipeline or script execution here.

#include <vinc/shell/detail/command_line.hpp>

namespace vinc::shell {
namespace {
bool is_separator(char character) {
    return character == ' ' || character == '\t';
}

std::size_t previous_code_point(std::string_view text, std::size_t end) {
    if (!end) {
        return 0;
    }

    --end;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }

    return end;
}
}

Result<ParsedLine> parse_line(std::string_view input, bool completion) {
    if (input.size() > max_line_bytes) {
        return std::unexpected(Error{ErrorCode::LineTooLong, ErrorOperation::ParseLine});
    }

    ParsedLine parsed;
    char quote = 0;
    bool has_argument = false;
    std::string argument;
    auto finish_argument = [&] {
        if (has_argument) {
            parsed.arguments.push_back(std::move(argument));
            argument.clear();
            has_argument = false;
        }
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char character = input[i];
        if (static_cast<unsigned char>(character) < 0x20 && character != '\t') {
            return std::unexpected(Error{ErrorCode::InvalidCharacter, ErrorOperation::ParseLine});
        }

        if (!quote && is_separator(character)) {
            finish_argument();
            parsed.trailing_space = true;
            continue;
        }

        parsed.trailing_space = false;
        if (!has_argument) {
            has_argument = true;
            parsed.last_start = i;
        }

        if (quote == '\'') {
            if (character == '\'') {
                quote = 0;
            } else {
                argument += character;
            }
        } else if (character == '\\') {
            bool escape_next = false;
            if (i + 1 < input.size()) {
                const char next_character = input[i + 1];
                const bool escaped_quote_or_slash = next_character == '\\' || next_character == '"';
                const bool escaped_unquoted_separator =
                    !quote && (next_character == '\'' || is_separator(next_character));
                escape_next = escaped_quote_or_slash || escaped_unquoted_separator;
            }

            if (escape_next) {
                ++i;
                argument += input[i];
            } else {
                // Preserve ordinary Windows path separators, including a final one.
                argument += character;
            }
        } else if (quote == '"') {
            if (character == '"') {
                quote = 0;
            } else {
                argument += character;
            }
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else {
            argument += character;
        }
    }

    if (quote && !completion) {
        return std::unexpected(Error{ErrorCode::UnterminatedQuote, ErrorOperation::ParseLine});
    }

    finish_argument();
    if (parsed.arguments.size() > config::shell_max_arguments) {
        return std::unexpected(Error{ErrorCode::TooManyArguments, ErrorOperation::ParseLine});
    }

    if (parsed.trailing_space) {
        parsed.last_start = input.size();
    }

    return parsed;
}

std::string quote_argument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\\\"'") == std::string_view::npos) {
        return std::string(argument);
    }

    std::string quoted = "\"";
    for (const auto character : argument) {
        if (character == '"' || character == '\\') {
            quoted += '\\';
        }

        quoted += character;
    }

    return quoted + '"';
}

bool LineEditor::append(std::string_view text) {
    if (text.size() > max_line_bytes - line_.size()) {
        return false;
    }

    line_ += text;
    return true;
}

bool LineEditor::replace(std::string value) {
    if (value.size() > max_line_bytes) {
        return false;
    }

    line_ = std::move(value);
    return true;
}

void LineEditor::backspace() {
    line_.resize(previous_code_point(line_, line_.size()));
}

void LineEditor::older() {
    if (history_.empty() || !position_) {
        return;
    }

    if (position_ == history_.size()) {
        draft_ = line_;
    }

    --position_;
    line_ = history_[position_];
}

void LineEditor::newer() {
    if (position_ >= history_.size()) {
        return;
    }

    ++position_;
    line_ = position_ == history_.size() ? draft_ : history_[position_];
}

std::string LineEditor::submit() {
    auto result = std::move(line_);
    line_.clear();
    if (!result.empty() && (history_.empty() || history_.back() != result)) {
        if (history_.size() == config::shell_history_entries) {
            history_.erase(history_.begin());
        }

        history_.push_back(result);
    }

    position_ = history_.size();
    draft_.clear();
    return result;
}

std::string LineEditor::visible_tail(std::size_t columns) const {
    auto begin = line_.size();
    std::size_t used = 0;
    while (begin) {
        const auto previous_start = previous_code_point(line_, begin);
        const std::size_t width = static_cast<unsigned char>(line_[previous_start]) < 128 ? 1 : 2;
        if (used + width > columns) {
            break;
        }

        begin = previous_start;
        used += width;
    }

    return line_.substr(begin);
}
}
