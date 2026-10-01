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
std::atomic<bool> acquired{false};
#ifdef _WIN32
volatile LONG interrupted = 0;
BOOL WINAPI control_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&interrupted, 1); return TRUE;
}
OsError native_error(Operation op) { return {ErrorCode::SystemError, op, {static_cast<int>(GetLastError()), std::system_category()}}; }
#else
volatile std::sig_atomic_t interrupted = 0;
void control_handler(int) { interrupted = 1; }
OsError native_error(Operation op) { return {ErrorCode::SystemError, op, {errno, std::generic_category()}}; }
#endif
std::string utf8(std::uint32_t value) {
    std::string out;
    if (value < 0x80) out += static_cast<char>(value);
    else if (value < 0x800) { out += static_cast<char>(0xc0 | (value >> 6)); out += static_cast<char>(0x80 | (value & 63)); }
    else if (value < 0x10000) { out += static_cast<char>(0xe0 | (value >> 12)); out += static_cast<char>(0x80 | ((value >> 6) & 63)); out += static_cast<char>(0x80 | (value & 63)); }
    else { out += static_cast<char>(0xf0 | (value >> 18)); out += static_cast<char>(0x80 | ((value >> 12) & 63)); out += static_cast<char>(0x80 | ((value >> 6) & 63)); out += static_cast<char>(0x80 | (value & 63)); }
    return out;
}
}
struct Terminal::Impl {
    bool console = false, ansi = false, ended = false, handler = false;
    bool previous_cr = false;
    std::string bytes;
    Deadline escape_deadline{};
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD input_mode = 0, output_mode = 0, kind = 0;
    UINT old_output_cp = 0;
    bool input_changed = false, output_changed = false;
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
        if (input_changed) SetConsoleMode(input, input_mode);
        if (output_changed) SetConsoleMode(output, output_mode);
        if (old_output_cp) SetConsoleOutputCP(old_output_cp);
        if (handler) SetConsoleCtrlHandler(control_handler, FALSE);
#else
        if (input_changed) tcsetattr(STDIN_FILENO, TCSANOW, &original);
        if (handler) sigaction(SIGINT, &original_signal, nullptr);
#endif
        acquired.store(false);
    }
    std::optional<TerminalEvent> decode() {
        if (bytes.empty()) return {};
        const auto c = static_cast<unsigned char>(bytes[0]);
        if (previous_cr) { previous_cr = false; if (c == '\n') { bytes.erase(0, 1); return decode(); } }
        if (c == 27) {
            if (escape_deadline == Deadline{}) escape_deadline = Clock::now() + 40ms;
            if (bytes.size() < 2 && !ended && Clock::now() < escape_deadline) return {};
            std::size_t length = 1;
            TerminalKey key = TerminalKey::Escape;
            if (bytes.size() >= 2 && (bytes[1] == '[' || bytes[1] == 'O')) {
                auto end = bytes.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~", 2);
                if (end == std::string::npos && bytes.size() < 32 && !ended && Clock::now() < escape_deadline) return {};
                length = end == std::string::npos ? bytes.size() : end + 1;
                if (length == 3 && bytes[2] == 'A') key = TerminalKey::Up;
                if (length == 3 && bytes[2] == 'B') key = TerminalKey::Down;
            }
            bytes.erase(0, length); escape_deadline = {}; return TerminalEvent{key, {}};
        }
        if (c < 0x20 || c == 0x7f) {
            bytes.erase(0, 1);
            switch (c) {
            case 3: return TerminalEvent{TerminalKey::Interrupt, {}};
            case 4: return TerminalEvent{TerminalKey::End, {}};
            case '\r': previous_cr = true; [[fallthrough]];
            case '\n': return TerminalEvent{TerminalKey::Enter, {}};
            case '\t': return TerminalEvent{TerminalKey::Tab, {}};
            case 8: case 127: return TerminalEvent{TerminalKey::Backspace, {}};
            default: return TerminalEvent{};
            }
        }
        const std::size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (n && bytes.size() < n && !ended) return {};
        bool valid = n && bytes.size() >= n;
        std::uint32_t value = n == 1 ? c : c & ((1U << (7 - n)) - 1);
        for (std::size_t i = 1; valid && i < n; ++i) {
            const auto b = static_cast<unsigned char>(bytes[i]);
            valid = (b & 0xc0) == 0x80; value = (value << 6) | (b & 63);
        }
        valid = valid && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff) &&
            (n < 2 || value >= 0x80) && (n < 3 || value >= 0x800) && (n < 4 || value >= 0x10000);
        auto text = valid ? bytes.substr(0, n) : utf8(0xfffd);
        bytes.erase(0, valid ? n : 1); return TerminalEvent{TerminalKey::Text, std::move(text)};
    }
};
Terminal::Terminal(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Terminal::~Terminal() = default;
Result<std::unique_ptr<Terminal>> Terminal::open() {
    if (acquired.exchange(true)) return std::unexpected(OsError{ErrorCode::Busy, Operation::OpenTerminal});
    std::unique_ptr<Impl> p;
    try { p = std::make_unique<Impl>(); }
    catch (...) { acquired.store(false); throw; }
#ifdef _WIN32
    InterlockedExchange(&interrupted, 0);
    if (!SetConsoleCtrlHandler(control_handler, TRUE)) return std::unexpected(native_error(Operation::OpenTerminal));
    p->handler = true;
    p->console = GetConsoleMode(p->input, &p->input_mode) != 0;
    if (p->console) {
        const auto mode = (p->input_mode | ENABLE_EXTENDED_FLAGS) & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!SetConsoleMode(p->input, mode)) return std::unexpected(native_error(Operation::OpenTerminal));
        p->input_changed = true;
    } else {
        p->kind = GetFileType(p->input);
        if (p->kind == FILE_TYPE_UNKNOWN) p->ended = true;
    }
    if (GetConsoleMode(p->output, &p->output_mode)) {
        p->old_output_cp = GetConsoleOutputCP();
        if (!SetConsoleOutputCP(CP_UTF8)) return std::unexpected(native_error(Operation::OpenTerminal));
        if (!SetConsoleMode(p->output, p->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return std::unexpected(native_error(Operation::OpenTerminal));
        p->output_changed = true; p->ansi = true;
    }
#else
    interrupted = 0;
    struct sigaction action{}; action.sa_handler = control_handler; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &p->original_signal)) return std::unexpected(native_error(Operation::OpenTerminal));
    p->handler = true;
    p->console = isatty(STDIN_FILENO) != 0; p->ansi = isatty(STDOUT_FILENO) != 0;
    if (p->console) {
        if (tcgetattr(STDIN_FILENO, &p->original)) return std::unexpected(native_error(Operation::OpenTerminal));
        auto mode = p->original; mode.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        mode.c_cc[VMIN] = 1; mode.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode)) return std::unexpected(native_error(Operation::OpenTerminal));
        p->input_changed = true;
    }
#endif
    return std::unique_ptr<Terminal>(new Terminal(std::move(p)));
}
bool Terminal::interactive() const noexcept { return impl_->console; }
bool Terminal::ansi_output() const noexcept { return impl_->ansi; }
std::size_t Terminal::columns() const noexcept {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(impl_->output, &info)) return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
#else
    winsize size{};
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col) return size.ws_col;
#endif
    return 80;
}
Result<TerminalEvent> Terminal::read_until(Deadline deadline) {
    auto& p = *impl_;
    for (;;) {
#ifdef _WIN32
        if (InterlockedExchange(&interrupted, 0)) return TerminalEvent{TerminalKey::Interrupt, {}};
        if (p.repeats) { --p.repeats; return *p.repeat; }
#else
        if (interrupted) { interrupted = 0; return TerminalEvent{TerminalKey::Interrupt, {}}; }
#endif
        if (auto event = p.decode()) return *event;
        if (p.ended) return TerminalEvent{TerminalKey::End, {}};
        const auto now = Clock::now(); if (now >= deadline) return TerminalEvent{};
        const auto remaining = (std::min)(10ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + 1ms);
#ifdef _WIN32
        if (p.console) {
            const auto wait = WaitForSingleObject(p.input, static_cast<DWORD>(remaining.count()));
            if (wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0) return std::unexpected(native_error(Operation::ReadTerminal));
            INPUT_RECORD record{}; DWORD read = 0;
            if (!ReadConsoleInputW(p.input, &record, 1, &read)) return std::unexpected(native_error(Operation::ReadTerminal));
            if (!read || record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
            const auto& k = record.Event.KeyEvent; TerminalEvent event;
            switch (k.wVirtualKeyCode) {
            case VK_RETURN: event.key = TerminalKey::Enter; break;
            case VK_BACK: event.key = TerminalKey::Backspace; break;
            case VK_UP: event.key = TerminalKey::Up; break;
            case VK_DOWN: event.key = TerminalKey::Down; break;
            case VK_TAB: event.key = TerminalKey::Tab; break;
            case VK_ESCAPE: event.key = TerminalKey::Escape; break;
            default: {
                std::uint32_t c = k.uChar.UnicodeChar;
                if (c == 3) event.key = TerminalKey::Interrupt;
                else if (c == 4 || c == 26) event.key = TerminalKey::End;
                else if (c >= 0xd800 && c <= 0xdbff) { p.surrogate = static_cast<WCHAR>(c); continue; }
                else if (c >= 0xdc00 && c <= 0xdfff) {
                    c = p.surrogate ? 0x10000 + ((p.surrogate - 0xd800) << 10) + c - 0xdc00 : 0xfffd;
                    p.surrogate = 0; event = {TerminalKey::Text, utf8(c)};
                } else if (c >= 0x20 && c != 0x7f) { p.surrogate = 0; event = {TerminalKey::Text, utf8(c)}; }
                break;
            }
            }
            p.repeat = event; p.repeats = k.wRepeatCount ? static_cast<WORD>(k.wRepeatCount - 1) : 0;
            return event;
        }
        if (p.kind == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(p.input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { p.ended = true; continue; }
                return std::unexpected(native_error(Operation::ReadTerminal));
            }
            if (!available) { std::this_thread::sleep_for(remaining); continue; }
        }
        char bytes[256]; DWORD count = 0;
        if (!ReadFile(p.input, bytes, sizeof(bytes), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF) { p.ended = true; continue; }
            return std::unexpected(native_error(Operation::ReadTerminal));
        }
#else
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        const auto status = poll(&fd, 1, static_cast<int>(remaining.count()));
        if (status < 0) { if (errno == EINTR) continue; return std::unexpected(native_error(Operation::ReadTerminal)); }
        if (!status) continue;
        char bytes[256]; const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0) { if (errno == EINTR || errno == EAGAIN) continue; return std::unexpected(native_error(Operation::ReadTerminal)); }
#endif
        if (!count) p.ended = true;
        else p.bytes.append(bytes, static_cast<std::size_t>(count));
    }
}
}
