#include "../../../../tests/support/manager.hpp"
// Run Shell against real module helpers while feeding controlled terminal input.
// Verify responsive commands, late replies and shutdown that waits for actual child exit.

#include <vinc/time.hpp>
#include "input_pipe.hpp"
#include <vinc/osal/terminal.hpp>
#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <atomic>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
using namespace vinc;
using namespace std::chrono_literals;

namespace {
// Test timing budgets and observation delays; chrono suffixes specify the units.
constexpr auto partial_input_observation_delay = 150ms;
constexpr auto crash_command_timeout = 2s;
constexpr auto restart_shutdown_timeout = 5s;
constexpr auto slow_command_timeout = 30ms;
constexpr auto command_probe_timeout = 1s;
constexpr auto shutdown_timeout_probe = 20ms;
constexpr auto shutdown_completion_timeout = 4s;
constexpr auto interrupt_input_settle_delay = 100ms;
} // namespace

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

// Bound asynchronous fixture polling so a failed scenario cannot hang CTest.
constexpr auto fixture_wait_timeout = 5s;
constexpr auto fixture_poll_interval = 10ms;

template <class Predicate> bool eventually(Predicate predicate) {
    const auto end = control::Clock::now() + fixture_wait_timeout;
    do {
        if (predicate()) {
            return true;
        }

        std::this_thread::sleep_for(fixture_poll_interval);
    } while (control::Clock::now() < end);
    return false;
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    // char may read UTF-8 char8_t object bytes; copy while the owning string remains alive.
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct Workspace {
    const std::filesystem::path root = std::filesystem::absolute(std::filesystem::current_path());
    std::filesystem::path path =
        root / ("shell-test-" + std::to_string(control::Clock::now().time_since_epoch().count()));

    Workspace() {
        check(std::filesystem::create_directory(path), "create test directory");
    }

    ~Workspace() {
        if (path.parent_path() == root) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

void register_helper(ModuleManager& manager, const std::filesystem::path& executable) {
    ModulePolicy policy;
    policy.heartbeat_interval = 80ms;
    policy.heartbeat_timeout = 1200ms;
    policy.startup_timeout = 5s;
    policy.stop_timeout = 1500ms;
    check(bool(manager.register_module_type({"shellHelper", executable, policy})), "register helper");
}

std::string start(ModuleManager& manager, const Workspace& files, const std::string& mode,
                  const std::string& module_name = "node") {
    auto name = manager.start_module("shellHelper", {"--name", module_name, mode, utf8(files.path)});
    check(bool(name), "start helper");
    check(bool(manager.wait_started(*name)), "wait helper ready");
    return manager.inspect(*name)->name;
}

void test_output_and_prompt(const std::filesystem::path& executable) {
    Workspace files;
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    register_helper(manager, executable);
    InputPipe input;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    const auto module = start(manager, files, "output");
    std::atomic<bool> feeder_ok{true};
    std::jthread feeder([&] {
        try {
            input.send(module + " output\n");
            check(eventually([&] {
                      return std::filesystem::exists(files.path / (module + ".output"));
                  }),
                  "output command executed");
            // Leave an incomplete command while module output arrives.
            input.send("hel");
            std::this_thread::sleep_for(partial_input_observation_delay);
            input.send("p\nexit\n");
        } catch (...) {
            feeder_ok = false;
            input.end();
        }
    });
    check(shell.run(), "Shell run");
    feeder.join();
    check(feeder_ok, "output feeder");
    const auto text = output.str();
    check(text.find("startup-output") != std::string::npos && text.find("startup-error") != std::string::npos,
          "startup stdout/stderr not routed to Shell");
    check(text.find("command-output\\x1B[2J") != std::string::npos && text.find('\x1b') == std::string::npos,
          "module ANSI was executed or output lost");
    check(text.find("[Modules]") != std::string::npos, "module output corrupted incomplete command");
    const auto tail = text.find("stop-tail-without-newline");
    check(tail != std::string::npos && tail < text.find("All module processes exited;"),
          "shutdown lost or reordered output tail");
    check(manager.output_hub().stats().active_captures == 0, "shutdown retained captures");
}

void test_restart_output_generation(const std::filesystem::path& executable) {
    Workspace files;
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    ModulePolicy policy;
    policy.heartbeat_interval = 80ms;
    policy.heartbeat_timeout = 1200ms;
    policy.max_restarts = 1;
    policy.restart_delay = 100ms;
    check(bool(manager.register_module_type({"shellHelper", executable, policy})),
          "register restart helper");
    auto destination = manager.output_hub().subscribe();
    check(bool(destination), "restart output subscriber");
    const auto module = start(manager, files, "output");
    const auto first = manager.inspect(module)->generation;
    (void)manager.command(module, {"crash-output"}, control::Clock::now() + crash_command_timeout);
    check(eventually([&] {
              auto snapshot = manager.inspect(module);
              return snapshot && snapshot->generation != first && snapshot->state == ModuleState::Running;
          }),
          "restart became ready");
    const auto second = manager.inspect(module)->generation;
    check(bool(manager.wait_shutdown(control::Clock::now() + restart_shutdown_timeout)),
          "restart output shutdown");
    auto batch = destination->take(128);
    std::string first_bytes;
    std::string second_bytes;
    for (const auto& event : batch.events) {
        if (event->source->stream != output_hub::Stream::Stdout) {
            continue;
        }

        if (event->source->generation == first) {
            first_bytes += event->bytes;
        }

        if (event->source->generation == second) {
            second_bytes += event->bytes;
        }
    }

    check(first_bytes.find("startup-output") != std::string::npos &&
              first_bytes.find("crash-output") != std::string::npos &&
              second_bytes.find("startup-output") != std::string::npos,
          "restart output generation association");
}

void test_terminal_failure_waits_for_process(const std::filesystem::path& executable) {
    Workspace files;
    auto manager = checked_value(vinc::test::create_manager());
    register_helper(*manager, executable);
    const auto module = start(*manager, files, "normal");
    InputPipe input;
    auto occupied_terminal = checked_value(osal::Terminal::open());
    std::ostringstream output;
    auto shell = checked_value(Shell::create(*manager, output));
    const auto result = shell->run();
    check(!result && result.error().is(shell::ErrorCode::TerminalFailed) &&
              result.error().operation == static_cast<std::int32_t>(shell::ErrorOperation::OpenTerminal) &&
              !result.error().causes().empty() && result.error().causes().front().domain == ErrorDomain::Osal,
          "terminal acquisition failure retains the OSAL cause");
    const auto snapshot = manager->inspect(module);
    check(snapshot && snapshot->last_exit.has_value() &&
              std::filesystem::exists(files.path / (module + ".stopped")),
          "run returns failure only after the owned process exits");
}

void test_command_timeout_and_shutdown(const std::filesystem::path& executable) {
    Workspace files;
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    register_helper(manager, executable);
    const auto module = start(manager, files, "slow-stop");
    shell::CommandRouter router(manager);
    check(router.initialize(), "initialize router");
    auto parsed = shell::parse_line(module + R"( check "中文 空白" "" "D:\Some Folder\a.txt")");
    check(parsed && router.execute(parsed->arguments).has_value() &&
              std::filesystem::exists(files.path / (module + ".checked")),
          "quoted arguments arrive unchanged at remote hook");
    check(checked_value(router.complete(module + " ")).choices == std::vector<std::string>{"alpha", "beta"},
          "remote parameter completion");
    auto timed = manager.command(module, {"slow"}, control::Clock::now() + slow_command_timeout);
    check(!timed && timed.error().is(vinc::module_system::ErrorCode::TimeoutOutcomeUnknown),
          "slow command timeout");
    auto busy = manager.command(module, {}, control::Clock::now() + command_probe_timeout);
    check(!busy && busy.error().is(vinc::module_system::ErrorCode::Busy),
          "timed out request is still in flight");
    const auto shell_busy = router.execute({module});
    check(!shell_busy && shell_busy.error().is(shell::ErrorCode::Busy) &&
              !shell_busy.error().causes().empty() &&
              shell_busy.error().causes().front().domain == ErrorDomain::ModuleSystem,
          "Shell exposes Busy and retains the ModuleSystem cause");
    check(eventually([&] {
              auto r = manager.command(module, {}, control::Clock::now() + command_probe_timeout);
              return r && *r == 0;
          }),
          "late reply releases busy state");
    manager.begin_shutdown();
    check(!manager.start_module("shellHelper"), "shutdown rejects starts");
    auto pending = manager.wait_shutdown(control::Clock::now() + shutdown_timeout_probe);
    check(!pending && pending.error().is(vinc::module_system::ErrorCode::Timeout),
          "shutdown timeout keeps supervision");
    check(bool(manager.wait_shutdown(control::Clock::now() + shutdown_completion_timeout)),
          "shutdown waits for actual exit");
    check(std::filesystem::exists(files.path / (module + ".stopped")),
          "on_stop completed before shutdown success");
    check(manager.inspect(module)->last_exit.has_value(), "OS exit observed");
}

void test_interrupt_with_hung_module(const std::filesystem::path& executable) {
    Workspace files;
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    register_helper(manager, executable);
    const auto normal = start(manager, files, "slow-stop");
    const auto hung = start(manager, files, "normal", "hungNode");
    InputPipe input;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    std::atomic<bool> feeder_ok{true};
    vinc::Deadline interrupted_at;
    std::jthread feeder([&] {
        try {
            input.send("unknown-command\n" + hung + " hang\n");
            check(eventually([&] {
                      return std::filesystem::exists(files.path / (hung + ".working"));
                  }),
                  "hang command entered");
            input.send(hung + " fail\nhelp\n" + normal + " fail\n");
            check(eventually([&] {
                      return std::filesystem::exists(files.path / (normal + ".failed-command"));
                  }),
                  "other module command executed while first is hung");
            std::this_thread::sleep_for(interrupt_input_settle_delay);
            interrupted_at = control::Clock::now();
            input.send("\x03\x03\x03");
        } catch (...) {
            feeder_ok.store(false);
            input.end();
        }
    });
    check(shell.run(), "Shell run");
    feeder.join();
    check(feeder_ok.load(), "input feeder");
    check(control::Clock::now() - interrupted_at >= 450ms, "Ctrl+C waits for slow normal stop");
    check(control::Clock::now() - interrupted_at < 5s, "Ctrl+C responsive while command is hung");
    check(std::filesystem::exists(files.path / (normal + ".stopped")), "normal process finished cleanup");
    check(!std::filesystem::exists(files.path / (hung + ".stopped")),
          "hung hook required forced termination");
    for (const auto& s : manager.snapshots()) {
        check(s.state == ModuleState::Stopped && s.last_exit.has_value(), "all process exits confirmed");
    }

    const auto text = output.str();
    check(text.find("Unknown command") != std::string::npos && text.find("returned 17") != std::string::npos,
          "interactive failures are displayed");
    check(text.find("Busy:") != std::string::npos && text.find("[Modules]") != std::string::npos,
          "busy reporting and help stay responsive");
    check(text.find("All module processes exited; Message Bus cleanup complete.") != std::string::npos,
          "shutdown completion displayed after confirmation");
}

void test_exit_and_eof(const std::filesystem::path& executable) {
    for (const bool eof : {false, true}) {
        Workspace files;
        auto manager_owner = vinc::test::create_manager();
        if (!manager_owner) {
            throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
        }
        auto& manager = **manager_owner;
        register_helper(manager, executable);
        const auto module = start(manager, files, "normal");
        InputPipe input;
        std::ostringstream output;
        auto shell_owner = checked_value(vinc::Shell::create(manager, output));
        auto& shell = *shell_owner;
        if (eof) {
            input.end();
        } else {
            input.send("exit\n");
        }

        check(shell.run(), "Shell run");
        check(std::filesystem::exists(files.path / (module + ".stopped")) &&
                  manager.inspect(module)->last_exit.has_value(),
              "exit and EOF use the same full shutdown");
    }
}

void test_contract_notification(const std::filesystem::path& executable) {
    Workspace files;
    auto manager = checked_value(vinc::test::create_manager());
    register_helper(*manager, executable);
    const auto module = start(*manager, files, "normal");
    shell::CommandRouter router(*manager);
    std::uint64_t cursor = 0;
    check(router.contract_notifications(cursor).empty(), "no speculative fault");
    (void)manager->command(module, {"contract"}, control::Clock::now() + crash_command_timeout);
    check(eventually([&] { return manager->inspect(module)->state == ModuleState::Failed; }), "fault reaped");
    const auto notification = router.contract_notifications(cursor);
    check(notification.find("[Module fault] node MID=") != std::string::npos &&
              notification.find("index=10 size=3") != std::string::npos, "central fault notification");
    check(router.contract_notifications(cursor).empty(), "notification cursor avoids duplicates");
    const auto help = checked_value(router.execute({"help"}));
    check(help.message.find("[Module fault] node MID=") != std::string::npos, "help retains diagnostic");
    check(!std::filesystem::exists(files.path / (module + ".stopped")), "no fatal cleanup hook");
    InputPipe input;
    std::ostringstream output;
    auto shell = checked_value(Shell::create(*manager, output));
    input.send("exit\n");
    check(shell->run(), "central Shell survives fault");
    check(output.str().find("[Module fault] node MID=") != std::string::npos,
          "local Shell automatically renders retained diagnostic");
}

void test_debug_gate_shutdown(const std::filesystem::path& executable) {
    auto manager = checked_value(vinc::test::create_manager());
    register_helper(*manager, executable);
    InputPipe input;
    std::ostringstream output;
    auto shell = checked_value(Shell::create(*manager, output));
    std::atomic<bool> feeder_ok{true};
    std::jthread feeder([&] {
        try {
            input.send("debug start shellHelper\n");
            check(eventually([&] {
                const auto snapshots = manager->snapshots();
                return !snapshots.empty() && snapshots.front().debug_waiting;
            }), "Shell debug start reaches pre-load gate");
            input.send("help\n");
            std::this_thread::sleep_for(interrupt_input_settle_delay);
            input.send("\x03");
        } catch (...) {
            feeder_ok = false;
            input.end();
        }
    });
    check(shell->run(), "Shell debug shutdown");
    feeder.join();
    check(feeder_ok, "debug input feeder");
    const auto snapshots = manager->snapshots();
    check(snapshots.size() == 1 && snapshots.front().last_exit.has_value(), "Ctrl+C reclaims waiting Host");
    check(output.str().find("DEBUG") != std::string::npos, "help remains usable at debug gate");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    try {
        check(shell::initialize_tcl(argv[0]), "initialize Tcl");
        const auto exe = std::filesystem::absolute(argv[1]);
        test_debug_gate_shutdown(exe);
        test_contract_notification(exe);
        test_output_and_prompt(exe);
        test_restart_output_generation(exe);
        test_command_timeout_and_shutdown(exe);
        test_terminal_failure_waits_for_process(exe);
        test_interrupt_with_hung_module(exe);
        test_exit_and_eof(exe);
        std::cout << "shell integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
