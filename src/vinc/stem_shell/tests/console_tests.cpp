// Windows-only native console tests in an isolated hidden console.
// Exercise real key/control events and headless Tcl deployment without changing the user's terminal.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vinc/osal/terminal.hpp>
#include <module/module_manager.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
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
// Move the executable and Tcl resources to a Unicode path, remove interactive stdin,
// and prove script errors leave the process alive until an OS shutdown event arrives.
void headless_deployment(Terminal& terminal, const std::filesystem::path& vinc_exe) {
    const auto directory = std::filesystem::current_path() / ("tcl-deploy-" + std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(directory), "create deployment fixture");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    // Test Tcl resource relocation independently of the existing build-time module catalog.
    std::filesystem::copy_file(vinc_exe, directory / "Vinc.exe");
    std::filesystem::copy(vinc_exe.parent_path() / "lib", directory / "lib", std::filesystem::copy_options::recursive);
    const auto script = directory / std::filesystem::path(u8"啟動 腳本.tcl");
    const auto marker = directory / "ready.txt";
    std::ofstream(script) << "set f [open [lindex $argv 0] w]\nputs $f [lindex $argv 1]\nclose $f\nerror deliberate-startup-error\n";
    auto text = [](const auto& p) { auto value = p.generic_u8string(); return std::string(reinterpret_cast<const char*>(value.data()), value.size()); };
    auto service = ProcessService::create(); check(bool(service), "deployment process service");
    SpawnOptions options; options.executable = directory / "Vinc.exe";
    options.arguments = {"-S", text(script), text(marker), "中文 空白"};
    options.working_directory = directory; options.stdin_mode = StdioMode::Null;
    auto process = service->spawn(options); check(bool(process), "start relocated headless Vinc");
    struct Reap { Process& process; ~Reap() { (void)process.terminate(); (void)process.wait_until(std::chrono::steady_clock::now() + 3s); } } reap{*process};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    check(std::filesystem::exists(marker), "Unicode -S path and argv reached Tcl");
    std::this_thread::sleep_for(150ms);
    std::string value; std::ifstream(marker) >> value; check(value == "中文", "Unicode CLI argument preserved");
    auto status = process->try_wait(); check(status && !*status, "startup error and stdin EOF leave framework running");
    check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "interrupt headless deployment");
    auto exit = process->wait_until(std::chrono::steady_clock::now() + 4s);
    check(exit && exit->kind == ExitKind::Code && exit->value == 0, "headless framework handles Ctrl+C normally");
    // Consume the same broadcast in this fixture's terminal.
    auto event = terminal.read_until(std::chrono::steady_clock::now() + 1s);
    check(event && event->key == TerminalKey::Interrupt, "fixture observes broadcast");
}
int child(const std::filesystem::path& vinc_exe) {
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
        INPUT_RECORD cancel{}; cancel.EventType = KEY_EVENT; cancel.Event.KeyEvent.bKeyDown = TRUE;
        cancel.Event.KeyEvent.wRepeatCount = 1; cancel.Event.KeyEvent.wVirtualKeyCode = 'D';
        cancel.Event.KeyEvent.uChar.UnicodeChar = 4;
        check(WriteConsoleInputW(input, &cancel, 1, &written), "inject Ctrl+D");
        check(next().key == TerminalKey::Cancel, "native Ctrl+D is distinct from EOF");
        broadcast_shutdown(*terminal);
        headless_deployment(*terminal, vinc_exe);
    }
    DWORD after_input = 0, after_output = 0;
    check(GetConsoleMode(input, &after_input) && GetConsoleMode(output, &after_output), "query restored console");
    check(after_input == before_input && after_output == before_output && GetConsoleOutputCP() == cp, "console modes and code page restored");
    return 0;
}
int main(int argc, char** argv) {
    try {
        if (argc == 3) return child(std::filesystem::absolute(argv[2]));
        if (argc != 2) return 2;
        std::wstring path(32768, L'\0'); const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        check(length > 0 && length < path.size(), "executable path"); path.resize(length);
        auto command = L"\"" + path + L"\" --child \"" + std::filesystem::absolute(argv[1]).wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process), "start hidden console test");
        struct Reap {
            PROCESS_INFORMATION p;
            ~Reap() { TerminateProcess(p.hProcess, 99); WaitForSingleObject(p.hProcess, 3000); CloseHandle(p.hThread); CloseHandle(p.hProcess); }
        } reap{process};
        check(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0, "console child completed");
        DWORD code = 0; check(GetExitCodeProcess(process.hProcess, &code) && code == 0, "native console verification");
        std::cout << "native console tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
