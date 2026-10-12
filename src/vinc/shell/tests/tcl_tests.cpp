#include "../../../../tests/support/manager.hpp"
#include <vinc/osal/path.hpp>
// Embed real Tcl to test language behavior, argument preservation and per-run isolation.
// Cancellation/error cases also verify that script failure does not implicitly stop live modules.

#include "input_pipe.hpp"
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <vinc/shell/detail/shell.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
// Test timing budgets and observation delays; chrono suffixes specify the units.
constexpr auto module_command_timeout = 1s;
constexpr auto module_shutdown_timeout = 3s;
constexpr auto startup_error_observation_delay = 150ms;
constexpr auto foreground_input_probe_delay = 80ms;
constexpr auto script_cancel_settle_delay = 150ms;
constexpr auto script_completion_settle_delay = 100ms;
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
    auto utf8 = path.generic_u8string();
    // char may read UTF-8 char8_t object bytes; copy while the owning string remains alive.
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

struct Files {
    std::filesystem::path path =
        std::filesystem::current_path() /
        ("tcl-test-" + std::to_string(control::Clock::now().time_since_epoch().count()));

    Files() {
        std::filesystem::create_directory(path);
    }

    ~Files() {
        std::error_code error_code;
        std::filesystem::remove_all(path, error_code);
    }

    std::filesystem::path write(const std::string& name, const std::string& contents) {
        auto file = path / checked_value(osal::path_from_utf8(name));
        std::ofstream(file, std::ios::binary) << contents;
        return file;
    }
};

void test_module_wait_cancellation(Files& files, shell::TclRuntime& runtime, char** argv) {
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    shell::CommandRouter router(manager);
    check(router.initialize(), "initialize router");
    ModulePolicy policy;
    policy.stop_timeout = 1s;
    check(bool(manager.register_module_type(
              {"shellHelper", std::filesystem::absolute(argv[1]), "waitingNode", policy})),
          "register waiting helper");
    auto name = manager.start_module("shellHelper", {"normal", utf8(files.path)});
    check(name && manager.wait_started(*name), "waiting helper ready");
    auto waiting = files.write("waiting.tcl", "waitingNode slow\nwaitingNode fail\n");
    check(runtime.start(waiting, {},
                        [&](const auto& args, std::stop_token stop) {
                            return router.execute(args, stop);
                        }),
          "start waiting script");
    check(eventually([&] {
              return std::filesystem::exists(files.path / "waitingNode.working");
          }),
          "remote command in progress");
    runtime.cancel();
    check(eventually([&] {
              return runtime.ready();
          }),
          "module wait is cancellable");
    const auto canceled = runtime.finish();
    check(!canceled.outcome && canceled.outcome.error().is(shell::ErrorCode::Cancelled),
          "module wait cancellation reported");
    auto busy = manager.command(*name, {}, control::Clock::now() + module_command_timeout);
    check(!busy && busy.error().is(vinc::module_system::ErrorCode::Busy),
          "cancel does not forget in-flight command");
    check(eventually([&] {
              return bool(manager.command(*name, {}, control::Clock::now() + module_command_timeout));
          }),
          "late response releases Busy");
    check(!std::filesystem::exists(files.path / "waitingNode.failed-command"),
          "no subsequent script command after cancellation");
    check(manager.inspect(*name)->state == ModuleState::Running, "cancel keeps module running");
    manager.begin_shutdown();
    check(bool(manager.wait_shutdown(control::Clock::now() + module_shutdown_timeout)),
          "waiting fixture cleanup");
}

void test_startup_error_returns_to_shell(Files& files, char** argv) {
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    const auto exe = std::filesystem::absolute(argv[1]);
    check(!manager.register_module_type({"set", exe, "valid", {}}), "Tcl module type name reserved");
    check(!manager.register_module_type({"valid", exe, "list", {}}),
          "Tcl module base reserved instead of adding suffix");
    ModulePolicy policy;
    policy.stop_timeout = 1s;
    policy.startup_timeout = 4s;
    check(bool(manager.register_module_type({"shellHelper", exe, "node", policy})), "register helper");
    auto startup = files.write(
        "deployment.tcl",
        "shellHelper normal {" + utf8(files.path) +
            "}\nnode check {中文 空白} {} {D:\\Some Folder\\a.txt}\nnode fail\nerror unreachable\n");
    InputPipe input;
    std::atomic<bool> ok{true};
    std::jthread feeder([&] {
        try {
            check(eventually([&] {
                      return std::filesystem::exists(files.path / "node.failed-command");
                  }),
                  "startup command executed");
            std::this_thread::sleep_for(startup_error_observation_delay);
            check(manager.inspect("node")->state == ModuleState::Running, "script error keeps module alive");
            input.send("help\nexit\n");
        } catch (...) {
            ok.store(false);
            input.send("\x03");
        }
    });
    check(shell.run(startup), "Shell run");
    feeder.join();
    check(ok.load(), "startup feeder");
    check(std::filesystem::exists(files.path / "node.checked"), "real module gets Tcl arguments unchanged");
    check(output.str().find("line 3") != std::string::npos &&
              output.str().find("[Modules]") != std::string::npos,
          "startup error returns to Shell");
    check(manager.inspect("node")->last_exit.has_value(), "exit waits for module process");
}

void test_foreground_script_cancellation(Files& files) {
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    InputPipe input;
    std::atomic<bool> entered_loop{false};
    std::atomic<bool> after_cancel{false};
    check(bool(manager.register_micro_program("loopStarted",
                                              [&](auto&) {
                                                  entered_loop.store(true);
                                                  return 0;
                                              })),
          "register loop marker");
    check(bool(manager.register_micro_program("afterCancel",
                                              [&](auto&) {
                                                  after_cancel.store(true);
                                                  return 0;
                                              })),
          "register completion marker");
    auto loop = files.write("前景 😀.tcl", "loopStarted\nwhile {1} {}\n");
    auto second = files.write("second.tcl", "afterCancel\n");
    std::atomic<bool> ok{true};
    std::jthread feeder([&] {
        try {
            input.send("srun " + shell::quote_argument(utf8(loop)) + "\n");
            check(eventually([&] {
                      return entered_loop.load();
                  }),
                  "foreground script running");
            input.send("afterCancel\n");
            std::this_thread::sleep_for(foreground_input_probe_delay);
            check(!after_cancel.load(), "foreground script suspends ordinary input");
            input.send("\x04");
            std::this_thread::sleep_for(script_cancel_settle_delay);
            input.send("srun " + shell::quote_argument(utf8(second)) + "\n");
            check(eventually([&] {
                      return after_cancel.load();
                  }),
                  "Shell accepts script after Ctrl+D");
            std::this_thread::sleep_for(script_completion_settle_delay);
            input.send("\x04\nexit\n");
        } catch (...) {
            ok.store(false);
            input.send("\x03");
        }
    });
    check(shell.run(), "Shell run");
    feeder.join();
    check(ok.load(), "Ctrl+D foreground feeder");
    check(output.str().find("Script canceled:") != std::string::npos &&
              output.str().find("No script is running") != std::string::npos,
          "Ctrl+D cancel and idle behavior");
}

void test_startup_interrupt(Files& files, char** argv) {
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    InputPipe input;
    std::atomic<bool> entered_loop{false};
    check(bool(manager.register_micro_program("loopStarted",
                                              [&](auto&) {
                                                  entered_loop.store(true);
                                                  return 0;
                                              })),
          "shutdown loop marker");
    ModulePolicy policy;
    policy.stop_timeout = 1s;
    check(bool(manager.register_module_type(
              {"shellHelper", std::filesystem::absolute(argv[1]), "shutdownNode", policy})),
          "shutdown helper");
    auto startup = files.write("interrupt-startup.tcl",
                               "shellHelper normal {" + utf8(files.path) + "}\nloopStarted\nwhile {1} {}\n");
    std::atomic<bool> ok{true};
    std::jthread feeder([&] {
        if (!eventually([&] {
                return entered_loop.load();
            })) {
            ok.store(false);
        }

        input.send("\x03");
    });
    check(shell.run(startup), "Shell run");
    feeder.join();
    check(ok.load(), "startup loop reached");
    check(manager.inspect("shutdownNode")->last_exit.has_value() &&
              std::filesystem::exists(files.path / "shutdownNode.stopped"),
          "Ctrl+C during startup waits for module cleanup");
}

void test_runtime_language_and_reuse(Files& files, shell::TclRuntime& runtime) {
    std::vector<std::vector<std::string>> commands;
    auto dispatch = [&](const std::vector<std::string>& args,
                        std::stop_token) -> vinc::Result<shell::CommandReply> {
        commands.push_back(args);
        if (!args.empty() && args.front() == "missing") {
            return std::unexpected(
                vinc::Error{shell::ErrorCode::UnknownCommand, shell::ErrorOperation::Execute});
        }

        if (!args.empty() && args.front() == "fail") {
            return shell::CommandReply{17, false, "remote returned 17"};
        }

        return shell::CommandReply{};
    };
    auto run = [&](const std::filesystem::path& path, std::vector<std::string> args = {}) {
        check(runtime.start(path, std::move(args), dispatch), "start script");
        check(eventually([&] {
                  return runtime.ready();
              }),
              "script completed within deadline");
        return runtime.finish();
    };
    files.write("include.tcl", "set included 42\nreturn\nerror unreachable\n");
    auto success =
        files.write("中文 😀 startup.tcl", R"(source [file join [file dirname [info script]] include.tcl]
if {$included != 42 || $argc != 2 || [lindex $argv 1] ne ""} {error arguments}
set marker 99
foreach value {1 2 3} { testNode set $value }
testNode check [lindex $argv 0] "" {D:\Some Folder\a.txt} {$literal [unchanged]; # //}
puts "你好"
puts -nonewline stdout done
if {![catch {fail} reason] || $reason ne "remote returned 17"} {error catch}
)");
    check(run(success, {"中文 空白", ""}).outcome.has_value(),
          "Tcl language, source, Unicode arguments and catch");
    check(commands.size() == 5 &&
              commands[3] == std::vector<std::string>{"testNode", "check", "中文 空白", "",
                                                      "D:\\Some Folder\\a.txt", "$literal [unchanged]; # //"},
          "arguments are not re-parsed by Shell");
    check(runtime.take_output() == "你好\ndone", "puts is captured without corrupting arguments");
    check(run(files.write("fresh.tcl", "if {[info exists marker]} {error leaked}\n")).outcome.has_value(),
          "each run gets fresh variables");
    const auto failed = run(files.write("error.tcl", "set a 1\nmissing\nerror unreachable\n"));
    check(!failed.outcome.has_value() && shell::script_text(failed).find("error.tcl") != std::string::npos &&
              shell::script_text(failed).find("line 2") != std::string::npos,
          "error includes file and line");
    check(!run(files.path / "absent.tcl").outcome.has_value(), "missing script error");
#ifdef _WIN32
    const std::filesystem::path invalid_path(std::wstring(1, static_cast<wchar_t>(0xd800)));
#else
    const std::filesystem::path invalid_path(std::string("\xff"));
#endif
    const auto invalid_report = run(invalid_path);
    check(!invalid_report.outcome && invalid_report.outcome.error().is(shell::ErrorCode::ScriptFailed),
          "invalid native script path returns Shell failure");
    const auto invalid_text = error_to_string(invalid_report.outcome.error());
    check(invalid_text.find("PathToUtf8") != std::string::npos &&
              invalid_text.find("Invalid Unicode encoding") != std::string::npos,
          "invalid path preserves OSAL cause");
    std::atomic<bool> entered{false};
    auto cancellation_dispatch = [&](const auto&, std::stop_token) {
        entered.store(true);
        return shell::CommandReply{};
    };
    for (const std::string code :
         {"ready\nwhile {1} {catch {while {1} {}}}\n", "ready\nafter 600000\nerror unreachable\n"}) {
        entered.store(false);
        check(runtime.start(files.write("cancel.tcl", code), {}, cancellation_dispatch), "start cancel case");
        check(eventually([&] {
                  return entered.load();
              }),
              "cancel case entered");
        const auto busy = runtime.start(success, {}, dispatch);
        check(!busy && busy.error().is(shell::ErrorCode::Busy), "second script returns Busy");
        const auto begin = control::Clock::now();
        check(runtime.cancel(), "cancel active script");
        check(eventually([&] {
                  return runtime.ready();
              }),
              "cancel completes");
        const auto canceled = runtime.finish();
        check(!canceled.outcome && canceled.outcome.error().is(shell::ErrorCode::Cancelled) &&
                  control::Clock::now() - begin < 1s,
              "unwind bypasses catch and wakes after");
    }

    const auto shutdown = run(files.write("exit.tcl", "catch {exit}\nerror unreachable\n"));
    check(shutdown.outcome && shutdown.outcome->shutdown, "exit requests framework shutdown and unwinds");
    check(run(files.write("again.tcl", "set x 1\n")).outcome.has_value(), "run after cancellation and exit");
}

void test_renamed_commands(Files& files, shell::TclRuntime& runtime) {
    std::atomic<bool> entered{false};
    auto dispatch = [&](const std::vector<std::string>& args, std::stop_token) -> Result<shell::CommandReply> {
        if (!args.empty() && args.front() == "help") {
            return shell::CommandReply{0, false, "renamed-help"};
        }
        if (!args.empty() && args.front() == "ready") {
            entered.store(true);
            return shell::CommandReply{};
        }
        return std::unexpected(Error{shell::ErrorCode::UnknownCommand, shell::ErrorOperation::Execute});
    };
    auto run = [&](const std::string& source) {
        check(runtime.start(files.write("rename.tcl", source), {}, dispatch), "start renamed commands");
        check(eventually([&] {
                  return runtime.ready();
              }), "renamed commands complete");
        return runtime.finish();
    };
    check(run("rename puts say\nsay renamed-output\nrename help guide\nguide\n").outcome,
          "renamed puts and help preserve semantics");
    auto text = runtime.take_output();
    check(text.find("renamed-output") != std::string::npos && text.find("renamed-help") != std::string::npos,
          "renamed output remains captured");
    const auto nested = run("rename srun launch\nlaunch other.tcl\n");
    check(!nested.outcome && nested.outcome.error().is(shell::ErrorCode::Busy), "renamed srun remains Busy");
    const auto canceled = run("rename scancel cancelNow\ncatch {cancelNow}\nerror unreachable\n");
    check(!canceled.outcome && canceled.outcome.error().is(shell::ErrorCode::Cancelled),
          "renamed cancel still unwinds");
    check(runtime.start(files.write("renamed-after.tcl", "rename after pause\nready\npause 600000\n"), {}, dispatch),
          "start renamed after");
    check(eventually([&] {
                  return entered.load();
              }), "renamed after entered");
    runtime.cancel();
    check(eventually([&] {
                  return runtime.ready();
              }), "renamed after cancellable");
    check(!runtime.finish().outcome, "renamed after reports cancellation");
    const auto exited = run("rename exit quit\ncatch {quit}\nerror unreachable\n");
    check(exited.outcome && exited.outcome->shutdown, "renamed exit requests framework shutdown");
}

void test_framework_error_options(Files& files, shell::TclRuntime& runtime) {
    // Unknown numeric identifiers and the signed native value must survive Tcl's
    // return options. The chain is deliberately longer than the local capacity.
    auto failure = Error::from_raw(static_cast<ErrorDomain>(4000000000u), -1234, 5678,
                                   {static_cast<NativeErrorSource>(4000000001u), -1234567890123});
    for (std::size_t index = 0; index < config::error_chain_max_depth; ++index) {
        failure = wrap_error(shell::ErrorCode::CommandFailed, shell::ErrorOperation::Execute, failure);
    }
    auto dispatch = [&](const auto&, std::stop_token) -> Result<shell::CommandReply> {
        return std::unexpected(failure);
    };
    auto run = [&](const std::string& script) {
        check(runtime.start(files.write("cause.tcl", script), {}, dispatch), "start cause-chain script");
        check(eventually([&] {
                  return runtime.ready();
              }),
              "cause-chain script completes");
        return runtime.finish();
    };

    const auto direct = run("broken\n");
    check(!direct.outcome && direct.outcome.error().truncated() &&
              error_to_string(direct.outcome.error()) == error_to_string(failure),
          "Tcl carries complete numeric causes and omission state");
    const auto rethrow = run("catch {broken} reason options\nreturn -options $options $reason\n");
    check(!rethrow.outcome && error_to_string(rethrow.outcome.error()) == error_to_string(failure),
          "return -options preserves framework cause");
    const auto handled = run("catch {broken}\nset ok 1\n");
    check(handled.outcome, "caught error does not poison a successful script");
    const auto unrelated = run("catch {broken}\nerror unrelated\n");
    check(!unrelated.outcome && unrelated.outcome.error().is(shell::ErrorCode::ScriptFailed) &&
              unrelated.outcome.error().causes().empty() && unrelated.diagnostic.line == 2 &&
              unrelated.diagnostic.details.find("unrelated") != std::string::npos,
          "a later ordinary Tcl error cannot inherit a handled framework failure");
    for (const auto* malformed : {"VINC 2 1 0 8 11 7 0 0", "VINC 1 99 0", "VINC 1 1 1 8 11 7 0 0",
                                  "VINC 1 1 0 4294967296 11 7 0 0", "VINC 1 1 0 8 nope 7 0 0"}) {
        const auto report = run(std::string("return -code error -errorcode {") + malformed + "} bad\n");
        check(!report.outcome && report.outcome.error().is(shell::ErrorCode::ScriptFailed),
              "invalid framework error options are ordinary script failures");
    }
    const auto bounded = run("error [string repeat x 400000]\n");
    check(!bounded.outcome && bounded.diagnostic.details.size() <= config::shell_script_result_bytes,
          "Tcl diagnostic storage is bounded");
    const auto nested = run("srun another.tcl\n");
    check(!nested.outcome && nested.outcome.error().is(shell::ErrorCode::Busy),
          "nested script launch retains the Shell Busy classification");
    const auto invalid_exit = run("exit extra\n");
    check(!invalid_exit.outcome && invalid_exit.outcome.error().is(shell::ErrorCode::InvalidArgument),
          "invalid builtin arguments do not become generic script failures");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    try {
        check(shell::initialize_tcl(argv[0]), "initialize Tcl");
        Files files;
        shell::TclRuntime runtime;
        test_framework_error_options(files, runtime);
        test_renamed_commands(files, runtime);
        test_runtime_language_and_reuse(files, runtime);
        test_module_wait_cancellation(files, runtime, argv);
        test_startup_error_returns_to_shell(files, argv);
        test_foreground_script_cancellation(files);
        test_startup_interrupt(files, argv);
        std::cout << "Tcl integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
