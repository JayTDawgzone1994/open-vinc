#include "input_pipe.hpp"
#include <vinc/osal/terminal.hpp>
#include <future>
#include <iostream>
#include <thread>
using namespace vinc::osal;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        InputPipe input;
        auto opened = Terminal::open(); check(bool(opened), "open redirected terminal");
        auto terminal = std::move(*opened);
        auto busy = Terminal::open(); check(!busy && busy.error().code == ErrorCode::Busy, "exclusive input ownership");
        check(!terminal->interactive(), "redirected input detected");
        auto next = [&] { auto event = terminal->read_until(std::chrono::steady_clock::now() + 2s); check(bool(event), "read event"); return *event; };
        input.send("\xe4");
        check(terminal->read_until(std::chrono::steady_clock::now() + 20ms)->key == TerminalKey::Idle, "partial UTF-8 waits with deadline");
        input.send("\xb8\xad\r\n\x1b[A\x1b[B\t\b\x03");
        check(next().text == "中", "UTF-8 assembled");
        check(next().key == TerminalKey::Enter, "CRLF is one Enter");
        check(next().key == TerminalKey::Up && next().key == TerminalKey::Down, "escape direction keys");
        check(next().key == TerminalKey::Tab && next().key == TerminalKey::Backspace, "edit keys");
        check(next().key == TerminalKey::Interrupt, "Ctrl+C is distinct from EOF");
        input.send("\x1b");
        const auto begin = std::chrono::steady_clock::now();
        check(next().key == TerminalKey::Escape && std::chrono::steady_clock::now() - begin < 500ms, "standalone Escape does not hang");
        input.send("\xff"); check(next().text == "\xef\xbf\xbd", "malformed UTF-8 replacement");
        input.end(); check(next().key == TerminalKey::End, "EOF does not become a key or spin");
        terminal.reset(); check(bool(Terminal::open()), "input ownership released");
        std::cout << "terminal input tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
