#include "../../../../tests/support/manager.hpp"
#include <vinc/osal/path.hpp>
// Windows-only native console tests in an isolated hidden console.
// Exercise real key/control events and headless Tcl deployment without changing the user's terminal.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vinc/osal/terminal.hpp>
#include <vinc/module/host/module_manager.hpp>
#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <sstream>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <vector>
using namespace vinc::osal;
using namespace std::chrono_literals;

namespace {
// Test timing budgets and observation delays; chrono suffixes specify the units.
constexpr auto redraw_reap_timeout = 2s;
constexpr auto output_fixture_exit_timeout = 3s;
constexpr auto redraw_output_settle_delay = 200ms;
constexpr auto broadcast_interrupt_timeout = 2s;
constexpr auto broadcast_shutdown_timeout = 4s;
constexpr auto headless_reap_timeout = 3s;
constexpr auto deployment_ready_timeout = 5s;
constexpr auto deployment_poll_interval = 10ms;
constexpr auto headless_stability_delay = 150ms;
constexpr auto headless_shutdown_timeout = 4s;
constexpr auto parent_terminal_probe_timeout = 1s;
constexpr auto console_input_timeout = 2s;
constexpr auto output_fixture_start_delay = 200ms;
} // namespace

// Win32 extended-length path buffer, measured in UTF-16 code units.
constexpr std::size_t executable_path_units = 32768;
constexpr DWORD console_child_timeout_ms = 15000;
constexpr DWORD forced_reap_timeout_ms = 3000;

template <class T> T checked_value(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    return std::move(*result);
}

template <class T> void check(const vinc::Result<T>& result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + vinc::error_to_string(result.error()));
    }
}

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void output_redraw() {
    // Use the real console for keyboard state and an ostream to inspect the
    // exact redraw sequence without scraping console pixels.
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream rendered;
    auto shell_owner = checked_value(vinc::Shell::create(manager, rendered));
    auto& shell = *shell_owner;
    auto service = ProcessService::create();
    check(bool(service), "redraw process service");
    std::wstring self(executable_path_units, L'\0');
    const auto length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    check(length > 0 && length < self.size(), "redraw fixture path");
    self.resize(length);
    SpawnOptions options;
    options.executable = self;
    options.arguments = {"--output-fixture"};
    auto child = manager.output_hub().spawn(*service, options, "fixture", 1);
    check(bool(child), "spawn redraw output");

    struct Reap {
        Process& process;

        ~Reap() {
            (void)process.terminate();
            (void)process.wait_until(std::chrono::steady_clock::now() + redraw_reap_timeout);
        }
    } reap{child->process};

    auto send = [](const std::wstring& text) {
        std::vector<INPUT_RECORD> keys;
        for (wchar_t ch : text) {
            INPUT_RECORD record{};
            record.EventType = KEY_EVENT;
            record.Event.KeyEvent.bKeyDown = TRUE;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.uChar.UnicodeChar = ch;
            if (ch == '\r') {
                record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            }

            keys.push_back(record);
        }

        DWORD count = 0;
        return WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), keys.data(),
                                  static_cast<DWORD>(keys.size()), &count) &&
               count == keys.size();
    };
    check(send(L"hel"), "queue partial command");
    std::atomic<bool> fed{false};
    std::jthread feeder([&] {
        auto status =
            child->process.wait_until(std::chrono::steady_clock::now() + output_fixture_exit_timeout);
        manager.output_hub().exited(child->capture);
        std::this_thread::sleep_for(redraw_output_settle_delay);
        fed = status && status->value == 0;
        if (!send(L"p\rexit\r")) {
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        }
    });
    check(shell.run(), "Shell run");
    feeder.join();
    const auto text = rendered.str();
    const auto output = text.find("interactive-output");
    check(fed && output != std::string::npos, "interactive output fixture");
    check(text.find("\r\x1b[2KVINC > hel", output) != std::string::npos,
          "partial input not redrawn after output");
    check(text.find("[Modules]", output) != std::string::npos, "redrawn command did not execute");
}

void broadcast_shutdown(Terminal& terminal) {
    const auto root = std::filesystem::absolute(std::filesystem::current_path());
    const auto directory = root / ("shell-console-" + std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(directory), "create broadcast test directory");

    struct Cleanup {
        std::filesystem::path root;
        std::filesystem::path directory;

        ~Cleanup() {
            if (directory.parent_path() == root) {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }
    } cleanup{root, directory};

    std::wstring executable(executable_path_units, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(length > 0 && length < executable.size(), "helper executable directory");
    executable.resize(length);
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    vinc::ModulePolicy policy;
    policy.stop_timeout = 1500ms;
    check(bool(manager.register_module_type(
              {"shellHelper", std::filesystem::path(executable).parent_path() / "shell_process_helper.dll",
               "consoleNode", policy})),
          "register console helper");
    const auto path = directory.u8string();
    // char may read UTF-8 char8_t object bytes; copy while the owning string remains alive.
    auto module = manager.start_module(
        "shellHelper", {"slow-stop", std::string(reinterpret_cast<const char*>(path.data()), path.size())});
    check(bool(module) && bool(manager.wait_started(*module)), "console helper ready");
    // This test runs in its own hidden console; never broadcasts to the user's console.
    check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "broadcast OS Ctrl+C");
    bool interrupted = false;
    const auto deadline = std::chrono::steady_clock::now() + broadcast_interrupt_timeout;
    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        auto event = terminal.read_until(deadline);
        check(bool(event), "read broadcast interrupt");
        interrupted = event->key == TerminalKey::Interrupt;
    }

    check(interrupted, "parent receives OS Ctrl+C");
    manager.begin_shutdown();
    check(bool(manager.wait_shutdown(std::chrono::steady_clock::now() + broadcast_shutdown_timeout)),
          "broadcast shutdown confirms exit");
    const auto status = manager.inspect(*module);
    check(status && status->last_exit && status->last_exit->kind == ExitKind::Code &&
              status->last_exit->value == 0,
          "child exits through framework, not default console handler");
    check(std::filesystem::exists(directory / (status->name + ".stopped")),
          "broadcast permits on_stop cleanup");
}

// Move the executable and Tcl resources to a Unicode path, remove interactive stdin,
// and prove script errors leave the process alive until an OS shutdown event arrives.
void headless_deployment(Terminal& terminal, const std::filesystem::path& vinc_exe) {
    auto directory_name = std::filesystem::path(u8"tcl-deploy-中文-😀-");
    directory_name += std::to_string(GetCurrentProcessId());
    const auto directory = std::filesystem::current_path() / directory_name;
    check(std::filesystem::create_directories(directory), "create deployment fixture");

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error_code;
            std::filesystem::remove_all(path, error_code);
        }
    } cleanup{directory};

    // Test Tcl resource relocation independently of the existing build-time module catalog.
    std::filesystem::copy_file(vinc_exe, directory / "Vinc.exe");
    std::filesystem::copy(vinc_exe.parent_path() / "lib", directory / "lib",
                          std::filesystem::copy_options::recursive);
    const auto script = directory / std::filesystem::path(u8"啟動 😀 腳本.tcl");
    const auto marker = directory / "ready.txt";
    std::ofstream(script) << "set f [open [lindex $argv 0] w]\nputs $f [lindex $argv 1]\nclose $f\nerror "
                             "deliberate-startup-error\n";
    auto text = [](const auto& path) {
        auto value = path.generic_u8string();
        // char may read UTF-8 char8_t object bytes; copy while the owning string remains alive.
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    };
    auto service = ProcessService::create();
    check(bool(service), "deployment process service");
    SpawnOptions options;
    options.executable = directory / "Vinc.exe";
    options.arguments = {"--shell-port", "0", "-S", text(script), text(marker), "中文 空白"};
    options.working_directory = directory;
    options.stdin_mode = StdioMode::Null;
    auto process = service->spawn(options);
    check(bool(process), "start relocated headless Vinc");

    struct Reap {
        Process& process;

        ~Reap() {
            (void)process.terminate();
            (void)process.wait_until(std::chrono::steady_clock::now() + headless_reap_timeout);
        }
    } reap{*process};

    const auto deadline = std::chrono::steady_clock::now() + deployment_ready_timeout;
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(deployment_poll_interval);
    }

    check(std::filesystem::exists(marker), "Unicode -S path and argv reached Tcl");
    std::this_thread::sleep_for(headless_stability_delay);
    std::string value;
    std::ifstream(marker) >> value;
    check(value == "中文", "Unicode CLI argument preserved");
    auto status = process->try_wait();
    check(status && !*status, "startup error and stdin EOF leave framework running");
    check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "interrupt headless deployment");
    auto exit = process->wait_until(std::chrono::steady_clock::now() + headless_shutdown_timeout);
    check(exit && exit->kind == ExitKind::Code && exit->value == 0,
          "headless framework handles Ctrl+C normally");
    // Consume the same broadcast in this fixture's terminal.
    auto event = terminal.read_until(std::chrono::steady_clock::now() + parent_terminal_probe_timeout);
    check(event && event->key == TerminalKey::Interrupt, "fixture observes broadcast");
}

int child(const std::filesystem::path& vinc_exe) {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD before_input = 0;
    DWORD before_output = 0;
    check(GetConsoleMode(input, &before_input) && GetConsoleMode(output, &before_output),
          "hidden console attached");
    const auto output_code_page = GetConsoleOutputCP();
    {
        auto opened = Terminal::open();
        check(bool(opened), "open console terminal");
        auto terminal = std::move(*opened);
        check(terminal->interactive() && terminal->ansi_output(), "console identified");
        std::vector<INPUT_RECORD> keys;
        auto key = [&](WORD virtual_key, WCHAR value) {
            INPUT_RECORD record{};
            record.EventType = KEY_EVENT;
            auto& event = record.Event.KeyEvent;
            event.bKeyDown = TRUE;
            event.wRepeatCount = 1;
            event.wVirtualKeyCode = virtual_key;
            event.uChar.UnicodeChar = value;
            keys.push_back(record);
        };
        key(0, L'中');
        key(0, 0xd83d);
        key(0, 0xde00);
        key(VK_UP, 0);
        key(VK_TAB, L'\t');
        key(VK_BACK, L'\b');
        key('C', 3);
        DWORD written = 0;
        check(WriteConsoleInputW(input, keys.data(), static_cast<DWORD>(keys.size()), &written) &&
                  written == keys.size(),
              "inject native input records");
        auto next = [&] {
            auto event = terminal->read_until(std::chrono::steady_clock::now() + console_input_timeout);
            check(bool(event), "native console read");
            return *event;
        };
        check(next().text == "中", "UTF-16 Chinese to UTF-8");
        check(next().text == "\xf0\x9f\x98\x80", "UTF-16 surrogate pair to UTF-8");
        check(next().key == TerminalKey::Up && next().key == TerminalKey::Tab &&
                  next().key == TerminalKey::Backspace,
              "native edit keys");
        check(next().key == TerminalKey::Interrupt, "native Ctrl+C input initiates shutdown event");
        INPUT_RECORD cancel{};
        cancel.EventType = KEY_EVENT;
        cancel.Event.KeyEvent.bKeyDown = TRUE;
        cancel.Event.KeyEvent.wRepeatCount = 1;
        cancel.Event.KeyEvent.wVirtualKeyCode = 'D';
        cancel.Event.KeyEvent.uChar.UnicodeChar = 4;
        check(WriteConsoleInputW(input, &cancel, 1, &written), "inject Ctrl+D");
        check(next().key == TerminalKey::Cancel, "native Ctrl+D is distinct from EOF");
        broadcast_shutdown(*terminal);
        headless_deployment(*terminal, vinc_exe);
    }

    output_redraw();
    DWORD after_input = 0;
    DWORD after_output = 0;
    check(GetConsoleMode(input, &after_input) && GetConsoleMode(output, &after_output),
          "query restored console");
    check(after_input == before_input && after_output == before_output &&
              GetConsoleOutputCP() == output_code_page,
          "console modes and code page restored");
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--output-fixture") {
            std::this_thread::sleep_for(output_fixture_start_delay);
            std::cout << "interactive-output\n" << std::flush;
            return 0;
        }

        if (argc == 4) {
            check(vinc::shell::initialize_tcl(argv[0]), "initialize Tcl");
            return child(std::filesystem::absolute(argv[2]));
        }

        if (argc != 2) {
            return 2;
        }

        std::wstring path(executable_path_units, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        check(length > 0 && length < path.size(), "executable path");
        path.resize(length);
        const auto diagnostic = std::filesystem::absolute("console-output-test-" +
                                                          std::to_string(GetCurrentProcessId()) + ".txt");

        struct RemoveDiagnostic {
            std::filesystem::path path;

            ~RemoveDiagnostic() {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        } cleanup{diagnostic};

        auto command = L"\"" + path + L"\" --child \"" + std::filesystem::absolute(argv[1]).wstring() +
                       L"\" \"" + diagnostic.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                             nullptr, nullptr, &startup, &process),
              "start hidden console test");

        struct Reap {
            PROCESS_INFORMATION process_info;

            ~Reap() {
                TerminateProcess(process_info.hProcess, 99);
                WaitForSingleObject(process_info.hProcess, forced_reap_timeout_ms);
                CloseHandle(process_info.hThread);
                CloseHandle(process_info.hProcess);
            }
        } reap{process};

        check(WaitForSingleObject(process.hProcess, console_child_timeout_ms) == WAIT_OBJECT_0,
              "console child completed");
        DWORD code = 0;
        check(GetExitCodeProcess(process.hProcess, &code), "console exit code");
        if (code != 0) {
            std::ifstream file(diagnostic);
            std::string detail((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            throw std::runtime_error("native console verification: " + detail);
        }

        std::cout << "native console tests passed\n";
    } catch (const std::exception& error) {
        if (argc == 4) {
            auto diagnostic_path = vinc::osal::path_from_utf8(argv[3]);
            if (diagnostic_path) {
                std::ofstream(*diagnostic_path) << error.what();
            }
        }

        std::cerr << error.what() << '\n';
        return 1;
    }
}
