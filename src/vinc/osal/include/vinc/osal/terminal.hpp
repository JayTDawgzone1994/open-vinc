#pragma once

// Translate console events or redirected bytes into portable Shell input events.
// Terminal owns temporary OS settings; Shell decides what cancel, interrupt and EOF mean.

#include <vinc/osal/error.hpp>
#include <memory>
#include <string>

namespace vinc::osal {
// Interrupt = Ctrl+C, Cancel = Ctrl+D, End = actual input EOF, Idle = no event yet.
enum class TerminalKey {
    Text,
    Enter,
    Backspace,
    Up,
    Down,
    Tab,
    Escape,
    Interrupt,
    Cancel,
    End,
    Idle
};

struct TerminalEvent {
    TerminalKey key = TerminalKey::Idle;
    std::string text; // one complete UTF-8 code point for Text
};

// Exclusive standard-input owner. Open/read/destruction are serialized by caller.
// Raw console settings are restored by destruction. Redirected input is also supported.
class Terminal {
public:
    static Result<std::unique_ptr<Terminal>> open();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Result<TerminalEvent> read_until(Deadline);
    bool interactive() const noexcept;
    bool ansi_output() const noexcept;
    std::size_t columns() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Terminal(std::unique_ptr<Impl>);
};
}
