#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vinc/osal/terminal.hpp>
#include <module/module_manager.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace vinc::osal;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void broadcast_shutdown(Terminal& terminal) {
    const auto root = std::filesystem::absolute(std::filesystem::current_path());
    const auto directory = root / ("shell-console-" + std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(directory), "create broadcast test directory");
    struct Cleanup {
        std::filesystem::path root, directory;
        ~Cleanup() { if (directory.parent_path() == root) { std::error_code e; std::filesystem::remove_all(directory, e); } }
    } cleanup{root, directory};
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(length > 0 && length < executable.size(), "helper executable directory"); executable.resize(length);
    vinc::ModuleManager manager;
    vinc::ModulePolicy policy; policy.stop_timeout = 1500ms;
    check(bool(manager.register_module({"shellHelper", std::filesystem::path(executable).parent_path() / "shell_process_helper.exe", "consoleNode", policy})), "register console helper");
    const auto path = directory.u8string();
    auto node = manager.start_module("shellHelper", {"slow-stop", std::string(reinterpret_cast<const char*>(path.data()), path.size())});
    check(bool(node) && bool(manager.wait_started(*node)), "console helper ready");
    // This test runs in its own hidden console; never broadcasts to the user's console.
    check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "broadcast OS Ctrl+C");
    bool interrupted = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        auto event = terminal.read_until(deadline); check(bool(event), "read broadcast interrupt");
        interrupted = event->key == TerminalKey::Interrupt;
    }
    check(interrupted, "parent receives OS Ctrl+C");
    manager.begin_shutdown();
    check(bool(manager.wait_shutdown(std::chrono::steady_clock::now() + 4s)), "broadcast shutdown confirms exit");
    const auto status = manager.inspect(*node);
    check(status && status->last_exit && status->last_exit->kind == ExitKind::Code && status->last_exit->value == 0,
        "child exits through framework, not default console handler");
    check(std::filesystem::exists(directory / (*node + ".stopped")), "broadcast permits on_stop cleanup");
}
int child() {
    const auto input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD before_input = 0, before_output = 0;
    check(GetConsoleMode(input, &before_input) && GetConsoleMode(output, &before_output), "hidden console attached");
    const auto cp = GetConsoleOutputCP();
    {
        auto opened = Terminal::open(); check(bool(opened), "open console terminal"); auto terminal = std::move(*opened);
        check(terminal->interactive() && terminal->ansi_output(), "console identified");
        std::vector<INPUT_RECORD> keys;
        auto key = [&](WORD vk, WCHAR value) {
            INPUT_RECORD record{}; record.EventType = KEY_EVENT; auto& event = record.Event.KeyEvent;
            event.bKeyDown = TRUE; event.wRepeatCount = 1; event.wVirtualKeyCode = vk; event.uChar.UnicodeChar = value;
            keys.push_back(record);
        };
        key(0, L'中'); key(0, 0xd83d); key(0, 0xde00); key(VK_UP, 0); key(VK_TAB, L'\t'); key(VK_BACK, L'\b'); key('C', 3);
        DWORD written = 0; check(WriteConsoleInputW(input, keys.data(), static_cast<DWORD>(keys.size()), &written) && written == keys.size(), "inject native input records");
        auto next = [&] { auto event = terminal->read_until(std::chrono::steady_clock::now() + 2s); check(bool(event), "native console read"); return *event; };
        check(next().text == "中", "UTF-16 Chinese to UTF-8");
        check(next().text == "\xf0\x9f\x98\x80", "UTF-16 surrogate pair to UTF-8");
        check(next().key == TerminalKey::Up && next().key == TerminalKey::Tab && next().key == TerminalKey::Backspace, "native edit keys");
        check(next().key == TerminalKey::Interrupt, "native Ctrl+C input initiates shutdown event");
        broadcast_shutdown(*terminal);
    }
    DWORD after_input = 0, after_output = 0;
    check(GetConsoleMode(input, &after_input) && GetConsoleMode(output, &after_output), "query restored console");
    check(after_input == before_input && after_output == before_output && GetConsoleOutputCP() == cp, "console modes and code page restored");
    return 0;
}
int main(int argc, char**) {
    try {
        if (argc == 2) return child();
        std::wstring path(32768, L'\0'); const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        check(length > 0 && length < path.size(), "executable path"); path.resize(length);
        auto command = L"\"" + path + L"\" --child";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process), "start hidden console test");
        struct Reap {
            PROCESS_INFORMATION p;
            ~Reap() { TerminateProcess(p.hProcess, 99); WaitForSingleObject(p.hProcess, 3000); CloseHandle(p.hThread); CloseHandle(p.hProcess); }
        } reap{process};
        check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "console child completed");
        DWORD code = 0; check(GetExitCodeProcess(process.hProcess, &code) && code == 0, "native console verification");
        std::cout << "native console tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
