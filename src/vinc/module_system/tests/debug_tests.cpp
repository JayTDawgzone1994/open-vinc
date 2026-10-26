#include "../../../../tests/support/manager.hpp"
// Exercise the production Host gate and explicit per-launch supervision policy.
#include <vinc/module/host/module_manager.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 8s;
constexpr auto heartbeat_interval = 40ms;
constexpr auto heartbeat_timeout = 300ms;
constexpr auto startup_timeout = 1000ms;
constexpr auto stop_timeout = 300ms;
constexpr auto pause_observation = 1500ms;
constexpr auto poll_interval = 10ms;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void require(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

void await_exit(ModuleManager& manager, MID mid) {
    const auto deadline = control::Clock::now() + test_timeout;
    while (!manager.inspect(mid)->last_exit) {
        check(control::Clock::now() < deadline, "Host did not exit");
        std::this_thread::sleep_for(poll_interval);
    }
}

#ifdef _WIN32
// Native debugger integration belongs only to this test. Production does not detect
// debugger presence or depend on Windows debug APIs. A pending breakpoint freezes
// all target threads, including its Socket worker and watchdog.
class DebugAttachment {
    DWORD pid_ = 0;
    DEBUG_EVENT pending_{};
    bool pending = false;

public:
    ~DebugAttachment() {
        if (pending) {
            ContinueDebugEvent(pending_.dwProcessId, pending_.dwThreadId, DBG_CONTINUE);
        }
        if (pid_) {
            DebugActiveProcessStop(pid_);
        }
    }

    void attach(std::uint64_t pid) {
        check(DebugActiveProcess(static_cast<DWORD>(pid)) != FALSE, "native debugger attach failed");
        pid_ = static_cast<DWORD>(pid);
        check(DebugSetProcessKillOnExit(FALSE) != FALSE, "configure debugger cleanup");
        const auto deadline = control::Clock::now() + test_timeout;
        while (control::Clock::now() < deadline) {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, static_cast<DWORD>(poll_interval.count()))) {
                continue;
            }
            pending_ = event;
            pending = true;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
                // Process/thread event handles belong to the debug session. Windows
                // releases them on exit/continue or detach; do not close them early.
            } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                CloseHandle(event.u.LoadDll.hFile);
            } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                       event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                return;
            }
            check(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) != FALSE,
                  "continue debugger event");
            pending = false;
        }
        check(false, "attach breakpoint not received");
    }

    void finish_forced_exit() {
        check(pending, "stop test must hold a breakpoint");
        check(ContinueDebugEvent(pending_.dwProcessId, pending_.dwThreadId, DBG_CONTINUE) != FALSE,
              "release debugger event after stop deadline");
        pending = false;
        const auto deadline = control::Clock::now() + test_timeout;
        while (control::Clock::now() < deadline) {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, static_cast<DWORD>(poll_interval.count()))) {
                continue;
            }
            pending_ = event;
            pending = true;
            if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                CloseHandle(event.u.LoadDll.hFile);
            }
            const bool exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
            // OSAL uses STATUS_CONTROL_C_EXIT for an explicit Windows termination.
            constexpr DWORD forced_exit_status = 0xc000013aUL;
            const bool forced = exited && event.u.ExitProcess.dwExitCode == forced_exit_status;
            check(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) != FALSE,
                  "finish termination event");
            pending = false;
            if (exited) {
                pid_ = 0;
                check(forced, "paused process was not terminated by central OS policy");
                return;
            }
        }
        check(false, "debugger did not receive process exit");
    }

    void detach() {
        check(pending, "debugger must hold attach breakpoint");
        check(ContinueDebugEvent(pending_.dwProcessId, pending_.dwThreadId, DBG_CONTINUE) != FALSE,
              "continue paused Host");
        pending = false;
        if (!DebugActiveProcessStop(pid_)) {
            throw std::runtime_error("detach debugger native error=" + std::to_string(GetLastError()));
        }
        pid_ = 0;
    }
};
#endif

void exercise(const std::filesystem::path& plugin) {
    auto manager = take(vinc::test::create_manager());
    ModulePolicy policy;
    policy.heartbeat_interval = heartbeat_interval;
    policy.heartbeat_timeout = heartbeat_timeout;
    policy.startup_timeout = startup_timeout;
    policy.stop_timeout = stop_timeout;
    policy.max_restarts = 2;
    require(manager->register_module_type({"helper", plugin, policy}));
    shell::CommandRouter router(*manager);
    require(router.initialize());

    const auto neighbor = take(manager->start_module("helper", {"--name", "neighbor"}));
    require(manager->wait_running(neighbor, control::Clock::now() + test_timeout));
    auto start = take(router.execute({"debug", "start", "helper"}));
    check(start.message.find("PID=") != std::string::npos, "Shell supplies debugger PID");
    const auto mid = manager->snapshots().back().mid;
    auto snapshot = manager->inspect(mid);
    check(snapshot->debug && snapshot->debug_waiting && snapshot->name.empty(), "gate precedes naming");
    check(!manager->continue_debug(mid + 1000), "unknown MID rejected");
    check(!router.execute({"debug", "continue", "-1"}), "invalid MID rejected");
    std::this_thread::sleep_for(pause_observation);
    check(!manager->inspect(mid)->last_exit, "gate exceeds startup deadline without termination");

#ifdef _WIN32
    DebugAttachment debugger;
    debugger.attach(snapshot->pid);
    std::this_thread::sleep_for(pause_observation);
    check(!manager->inspect(mid)->last_exit, "native breakpoint survives heartbeat expiry");
    check(take(manager->command(neighbor, {}, control::Clock::now() + test_timeout)) == 42,
          "other Module remains operational while debugger pauses its peer");
    debugger.detach();
#endif

    take(router.execute({"debug", "continue", std::to_string(mid)}));
    check(!manager->continue_debug(mid), "release cannot be replayed");
    require(manager->wait_running(mid, control::Clock::now() + test_timeout));
    check(take(manager->command(mid, {}, control::Clock::now() + test_timeout)) == 42,
          "Module hooks run after release");
#ifdef _WIN32
    debugger.attach(snapshot->pid);
    std::this_thread::sleep_for(pause_observation);
    check(manager->inspect(mid)->state == ModuleState::Running, "running Module survives native pause");
    debugger.detach();
    check(take(manager->command(mid, {}, control::Clock::now() + test_timeout)) == 42,
          "control channel remains usable after native resume");
#endif
    (void)manager->command(mid, {"crash"}, control::Clock::now() + test_timeout);
    await_exit(*manager, mid);
    std::this_thread::sleep_for(pause_observation);
    snapshot = manager->inspect(mid);
    check(snapshot && snapshot->restarts == 0 && snapshot->last_exit, "debug launch never auto-restarts");

    const auto waiting = take(manager->start_module("helper", {}, {.debug = true}));
    require(manager->wait_debug_ready(waiting, control::Clock::now() + test_timeout));
    take(router.execute({"debug", "stop", std::to_string(waiting)}));
    check(manager->inspect(waiting)->last_exit.has_value(), "stop before load exits Host");

#ifdef _WIN32
    const auto paused = take(manager->start_module("helper", {}, {.debug = true}));
    require(manager->wait_debug_ready(paused, control::Clock::now() + test_timeout));
    debugger.attach(manager->inspect(paused)->pid);
    // The caller's short wait expires; central supervision must keep its stop request.
    const auto stopping = manager->stop_module(paused, control::Clock::now() + poll_interval);
    check(!stopping && stopping.error().is(module_system::ErrorCode::Timeout), "paused stop caller times out");
    std::this_thread::sleep_for(pause_observation);
    debugger.finish_forced_exit();
    await_exit(*manager, paused);
#endif

    const auto hung = take(manager->start_module("helper", {"init-hang"}, {.debug = true}));
    require(manager->wait_debug_ready(hung, control::Clock::now() + test_timeout));
    require(manager->continue_debug(hung));
    std::this_thread::sleep_for(pause_observation);
    check(!manager->inspect(hung)->last_exit, "blocked startup remains inspectable in debug mode");
    manager->begin_shutdown();
    require(manager->wait_shutdown(control::Clock::now() + test_timeout));
    check(manager->inspect(hung)->last_exit.has_value(), "shutdown terminates blocked debug Module");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "Plugin path required");
        exercise(std::filesystem::path(argv[1]));
        std::cout << "Module debugging tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
