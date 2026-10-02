// Own terminal modes and decode input without running framework shutdown in an OS handler.
// Control handlers record an event; the Shell loop performs cancellation and cleanup.

#include <vinc/osal/terminal.hpp>
#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace vinc::osal {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Allow split escape sequences to arrive before interpreting a lone Escape key.
constexpr auto escape_sequence_timeout = 40ms;
constexpr std::size_t max_escape_sequence_bytes = 32;
// Polling bounds how long interrupt flags wait before the input loop checks them.
constexpr auto input_poll_interval = 10ms;
constexpr std::size_t input_chunk_bytes = 256;
constexpr std::size_t fallback_terminal_columns = 80;
std::atomic<bool> acquired{false};
#ifdef _WIN32
volatile LONG interrupted = 0;

BOOL WINAPI control_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}

OsError native_error(Operation operation) {
    return {ErrorCode::SystemError, operation, {static_cast<int>(GetLastError()), std::system_category()}};
}
#else
volatile std::sig_atomic_t interrupted = 0;

void control_handler(int) {
    interrupted = 1;
}

OsError native_error(Operation operation) {
    return {ErrorCode::SystemError, operation, {errno, std::generic_category()}};
}
#endif
std::string utf8(std::uint32_t value) {
    std::string encoded;
    if (value < 0x80) {
        encoded += static_cast<char>(value);
    } else if (value < 0x800) {
        encoded += static_cast<char>(0xc0 | (value >> 6));
        encoded += static_cast<char>(0x80 | (value & 63));
    } else if (value < 0x10000) {
        encoded += static_cast<char>(0xe0 | (value >> 12));
        encoded += static_cast<char>(0x80 | ((value >> 6) & 63));
        encoded += static_cast<char>(0x80 | (value & 63));
    } else {
        encoded += static_cast<char>(0xf0 | (value >> 18));
        encoded += static_cast<char>(0x80 | ((value >> 12) & 63));
        encoded += static_cast<char>(0x80 | ((value >> 6) & 63));
        encoded += static_cast<char>(0x80 | (value & 63));
    }

    return encoded;
}
}

struct Terminal::Impl {
    bool console = false;
    bool ansi = false;
    bool ended = false;
    bool handler = false;
    bool previous_cr = false;
    std::string bytes;
    Deadline escape_deadline{};
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    DWORD kind = 0;
    UINT old_output_cp = 0;
    bool input_changed = false;
    bool output_changed = false;
    WCHAR surrogate = 0;
    std::optional<TerminalEvent> repeat;
    WORD repeats = 0;
#else
    termios original{};
    struct sigaction original_signal{};
    bool input_changed = false;
#endif
    ~Impl() {
#ifdef _WIN32
        if (input_changed) {
            SetConsoleMode(input, input_mode);
        }

        if (output_changed) {
            SetConsoleMode(output, output_mode);
        }

        if (old_output_cp) {
            SetConsoleOutputCP(old_output_cp);
        }

        if (handler) {
            SetConsoleCtrlHandler(control_handler, FALSE);
        }
#else
        if (input_changed) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original);
        }

        if (handler) {
            sigaction(SIGINT, &original_signal, nullptr);
        }
#endif
        acquired.store(false);
    }

    std::optional<TerminalEvent> decode() {
        if (bytes.empty()) {
            return {};
        }

        const auto leading_byte = static_cast<unsigned char>(bytes[0]);
        if (previous_cr) {
            previous_cr = false;
            if (leading_byte == '\n') {
                bytes.erase(0, 1);
                return decode();
            }
        }

        // Escape sequences and control keys are handled before UTF-8 text.
        if (leading_byte == 27) {
            if (escape_deadline == Deadline{}) {
                escape_deadline = Clock::now() + escape_sequence_timeout;
            }

            if (bytes.size() < 2 && !ended && Clock::now() < escape_deadline) {
                return {};
            }

            std::size_t length = 1;
            TerminalKey key = TerminalKey::Escape;
            if (bytes.size() >= 2 && (bytes[1] == '[' || bytes[1] == 'O')) {
                auto sequence_end =
                    bytes.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~", 2);
                if (sequence_end == std::string::npos && bytes.size() < max_escape_sequence_bytes && !ended &&
                    Clock::now() < escape_deadline) {
                    return {};
                }
                length = sequence_end == std::string::npos ? bytes.size() : sequence_end + 1;
                if (length == 3 && bytes[2] == 'A') {
                    key = TerminalKey::Up;
                }

                if (length == 3 && bytes[2] == 'B') {
                    key = TerminalKey::Down;
                }
            }
            bytes.erase(0, length);
            escape_deadline = {};
            return TerminalEvent{key, {}};
        }

        if (leading_byte < 0x20 || leading_byte == 0x7f) {
            bytes.erase(0, 1);
            switch (leading_byte) {
            case 3:
                return TerminalEvent{TerminalKey::Interrupt, {}};
            case 4:
                return TerminalEvent{TerminalKey::Cancel, {}};
            case '\r':
                previous_cr = true;
                [[fallthrough]];
            case '\n':
                return TerminalEvent{TerminalKey::Enter, {}};
            case '\t':
                return TerminalEvent{TerminalKey::Tab, {}};
            case 8:
            case 127:
                return TerminalEvent{TerminalKey::Backspace, {}};
            default:
                return TerminalEvent{};
            }
        }
        // A zero length marks an invalid UTF-8 leading byte.
        std::size_t sequence_length = 0;
        if (leading_byte < 0x80) {
            sequence_length = 1;
        } else if (leading_byte >= 0xc2 && leading_byte <= 0xdf) {
            sequence_length = 2;
        } else if (leading_byte >= 0xe0 && leading_byte <= 0xef) {
            sequence_length = 3;
        } else if (leading_byte >= 0xf0 && leading_byte <= 0xf4) {
            sequence_length = 4;
        }

        if (sequence_length && bytes.size() < sequence_length && !ended) {
            return {};
        }

        // Validate continuation bytes, overlong encodings and Unicode scalar range.
        bool valid = sequence_length && bytes.size() >= sequence_length;
        std::uint32_t code_point =
            sequence_length == 1 ? leading_byte : leading_byte & ((1U << (7 - sequence_length)) - 1);
        for (std::size_t i = 1; valid && i < sequence_length; ++i) {
            const auto continuation_byte = static_cast<unsigned char>(bytes[i]);
            valid = (continuation_byte & 0xc0) == 0x80;
            code_point = (code_point << 6) | (continuation_byte & 63);
        }
        valid = valid && code_point <= 0x10ffff && !(code_point >= 0xd800 && code_point <= 0xdfff) &&
                (sequence_length < 2 || code_point >= 0x80) && (sequence_length < 3 || code_point >= 0x800) &&
                (sequence_length < 4 || code_point >= 0x10000);

        // Consume only the bad leading byte so later input remains decodable.
        auto text = valid ? bytes.substr(0, sequence_length) : utf8(0xfffd);
        bytes.erase(0, valid ? sequence_length : 1);
        return TerminalEvent{TerminalKey::Text, std::move(text)};
    }
};

Terminal::Terminal(std::unique_ptr<Impl> state) : impl_(std::move(state)) {
}

Terminal::~Terminal() = default;

Result<std::unique_ptr<Terminal>> Terminal::open() {
    if (acquired.exchange(true)) {
        return std::unexpected(OsError{ErrorCode::Busy, Operation::OpenTerminal});
    }

    // Allocation failure must release the process-wide terminal reservation.
    std::unique_ptr<Impl> state;
    try {
        state = std::make_unique<Impl>();
    } catch (...) {
        acquired.store(false);
        throw;
    }
#ifdef _WIN32
    InterlockedExchange(&interrupted, 0);
    if (!SetConsoleCtrlHandler(control_handler, TRUE)) {
        return std::unexpected(native_error(Operation::OpenTerminal));
    }
    state->handler = true;
    state->console = GetConsoleMode(state->input, &state->input_mode) != 0;
    if (state->console) {
        const auto mode = (state->input_mode | ENABLE_EXTENDED_FLAGS) &
                          ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                            ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!SetConsoleMode(state->input, mode)) {
            return std::unexpected(native_error(Operation::OpenTerminal));
        }
        state->input_changed = true;
    } else {
        state->kind = GetFileType(state->input);
        if (state->kind == FILE_TYPE_UNKNOWN) {
            state->ended = true;
        }
    }

    if (GetConsoleMode(state->output, &state->output_mode)) {
        state->old_output_cp = GetConsoleOutputCP();
        if (!SetConsoleOutputCP(CP_UTF8)) {
            return std::unexpected(native_error(Operation::OpenTerminal));
        }

        if (!SetConsoleMode(state->output, state->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            return std::unexpected(native_error(Operation::OpenTerminal));
        }
        state->output_changed = true;
        state->ansi = true;
    }
#else
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = control_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &state->original_signal)) {
        return std::unexpected(native_error(Operation::OpenTerminal));
    }
    state->handler = true;
    state->console = isatty(STDIN_FILENO) != 0;
    state->ansi = isatty(STDOUT_FILENO) != 0;
    if (state->console) {
        if (tcgetattr(STDIN_FILENO, &state->original)) {
            return std::unexpected(native_error(Operation::OpenTerminal));
        }

        auto mode = state->original;
        mode.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        mode.c_cc[VMIN] = 1;
        mode.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
            return std::unexpected(native_error(Operation::OpenTerminal));
        }
        state->input_changed = true;
    }
#endif
    return std::unique_ptr<Terminal>(new Terminal(std::move(state)));
}

bool Terminal::interactive() const noexcept {
    return impl_->console;
}

bool Terminal::ansi_output() const noexcept {
    return impl_->ansi;
}

std::size_t Terminal::columns() const noexcept {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(impl_->output, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
#else
    winsize size{};
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col) {
        return size.ws_col;
    }
#endif
    return fallback_terminal_columns;
}

Result<TerminalEvent> Terminal::read_until(Deadline deadline) {
    auto& state = *impl_;
    for (;;) {
#ifdef _WIN32
        if (InterlockedExchange(&interrupted, 0)) {
            return TerminalEvent{TerminalKey::Interrupt, {}};
        }

        if (state.repeats) {
            --state.repeats;
            return *state.repeat;
        }
#else
        if (interrupted) {
            interrupted = 0;
            return TerminalEvent{TerminalKey::Interrupt, {}};
        }
#endif
        if (auto event = state.decode()) {
            return *event;
        }

        if (state.ended) {
            return TerminalEvent{TerminalKey::End, {}};
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            return TerminalEvent{};
        }

        const auto remaining =
            (std::min)(input_poll_interval,
                       std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + 1ms);
#ifdef _WIN32
        if (state.console) {
            const auto wait = WaitForSingleObject(state.input, static_cast<DWORD>(remaining.count()));
            if (wait == WAIT_TIMEOUT) {
                continue;
            }

            if (wait != WAIT_OBJECT_0) {
                return std::unexpected(native_error(Operation::ReadTerminal));
            }
            INPUT_RECORD record{};
            DWORD read = 0;
            if (!ReadConsoleInputW(state.input, &record, 1, &read)) {
                return std::unexpected(native_error(Operation::ReadTerminal));
            }

            if (!read || record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }

            const auto& key_event = record.Event.KeyEvent;
            TerminalEvent event;
            switch (key_event.wVirtualKeyCode) {
            case VK_RETURN:
                event.key = TerminalKey::Enter;
                break;
            case VK_BACK:
                event.key = TerminalKey::Backspace;
                break;
            case VK_UP:
                event.key = TerminalKey::Up;
                break;
            case VK_DOWN:
                event.key = TerminalKey::Down;
                break;
            case VK_TAB:
                event.key = TerminalKey::Tab;
                break;
            case VK_ESCAPE:
                event.key = TerminalKey::Escape;
                break;
            default: {
                std::uint32_t code_point = key_event.uChar.UnicodeChar;
                if (code_point == 3) {
                    event.key = TerminalKey::Interrupt;
                } else if (code_point == 4) {
                    event.key = TerminalKey::Cancel;
                } else if (code_point == 26) {
                    event.key = TerminalKey::End;
                } else if (code_point >= 0xd800 && code_point <= 0xdbff) {
                    state.surrogate = static_cast<WCHAR>(code_point);
                    continue;
                } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
                    code_point = state.surrogate
                                     ? 0x10000 + ((state.surrogate - 0xd800) << 10) + code_point - 0xdc00
                                     : 0xfffd;
                    state.surrogate = 0;
                    event = {TerminalKey::Text, utf8(code_point)};
                } else if (code_point >= 0x20 && code_point != 0x7f) {
                    state.surrogate = 0;
                    event = {TerminalKey::Text, utf8(code_point)};
                }
                break;
            }
            }
            state.repeat = event;
            state.repeats = key_event.wRepeatCount ? static_cast<WORD>(key_event.wRepeatCount - 1) : 0;
            return event;
        }

        if (state.kind == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(state.input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    state.ended = true;
                    continue;
                }

                return std::unexpected(native_error(Operation::ReadTerminal));
            }

            if (!available) {
                std::this_thread::sleep_for(remaining);
                continue;
            }
        }
        char bytes[input_chunk_bytes];
        DWORD count = 0;
        if (!ReadFile(state.input, bytes, sizeof(bytes), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF) {
                state.ended = true;
                continue;
            }

            return std::unexpected(native_error(Operation::ReadTerminal));
        }
#else
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        const auto status = poll(&fd, 1, static_cast<int>(remaining.count()));
        if (status < 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected(native_error(Operation::ReadTerminal));
        }

        if (!status) {
            continue;
        }
        char bytes[input_chunk_bytes];
        const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }

            return std::unexpected(native_error(Operation::ReadTerminal));
        }
#endif
        if (!count) {
            state.ended = true;
        } else {
            state.bytes.append(bytes, static_cast<std::size_t>(count));
        }
    }
}
}
