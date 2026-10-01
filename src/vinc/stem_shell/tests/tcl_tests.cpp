// Embed real Tcl to test language behavior, argument preservation and per-run isolation.
// Cancellation/error cases also verify that script failure does not implicitly stop live modules.

#include "input_pipe.hpp"
#include <stem_shell/tcl_runtime.hpp>
#include <stem_shell/stem_shell.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> bool eventually(Predicate predicate) {
    const auto end = control::Clock::now() + 5s;
    do { if (predicate()) return true; std::this_thread::sleep_for(10ms); } while (control::Clock::now() < end);
    return false;
}
std::string utf8(const std::filesystem::path& path) {
    auto s = path.generic_u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
struct Files {
    std::filesystem::path path = std::filesystem::current_path() / ("tcl-test-" + std::to_string(control::Clock::now().time_since_epoch().count()));
    Files() { std::filesystem::create_directory(path); }
    ~Files() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path write(const std::string& name, const std::string& contents) {
        auto file = path / std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size()));
        std::ofstream(file, std::ios::binary) << contents; return file;
    }
};
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        shell::initialize_tcl(argv[0]); Files files; shell::TclRuntime runtime;
        std::vector<std::vector<std::string>> commands;
        auto dispatch = [&](const std::vector<std::string>& args, std::stop_token) {
            commands.push_back(args);
            if (!args.empty() && args.front() == "missing") return shell::CommandResult{false, false, "Unknown command: missing"};
            if (!args.empty() && args.front() == "fail") return shell::CommandResult{false, false, "remote returned 17"};
            return shell::CommandResult{};
        };
        auto run = [&](const std::filesystem::path& path, std::vector<std::string> args = {}) {
            check(runtime.start(path, std::move(args), dispatch), "start script");
            check(eventually([&] { return runtime.ready(); }), "script completed within deadline");
            return runtime.finish();
        };
        files.write("include.tcl", "set included 42\nreturn\nerror unreachable\n");
        auto success = files.write("中文 startup.tcl", R"(source [file join [file dirname [info script]] include.tcl]
if {$included != 42 || $argc != 2 || [lindex $argv 1] ne ""} {error arguments}
set marker 99
foreach value {1 2 3} { testNode set $value }
testNode check [lindex $argv 0] "" {D:\Some Folder\a.txt} {$literal [unchanged]; # //}
puts "你好"
puts -nonewline stdout done
if {![catch {fail} reason] || $reason ne "remote returned 17"} {error catch}
)");
        check(run(success, {"中文 空白", ""}).success, "Tcl language, source, Unicode arguments and catch");
        check(commands.size() == 5 && commands[3] == std::vector<std::string>{"testNode", "check", "中文 空白", "", "D:\\Some Folder\\a.txt", "$literal [unchanged]; # //"}, "arguments are not re-parsed by Shell");
        check(runtime.take_output() == "你好\ndone", "puts is captured without corrupting arguments");
        check(run(files.write("fresh.tcl", "if {[info exists marker]} {error leaked}\n")).success, "each run gets fresh variables");
        const auto failed = run(files.write("error.tcl", "set a 1\nmissing\nerror unreachable\n"));
        check(!failed.success && failed.message.find("error.tcl") != std::string::npos && failed.message.find("line 2") != std::string::npos, "error includes file and line");
        check(!run(files.path / "absent.tcl").success, "missing script error");
        std::atomic<bool> entered{false};
        auto cancellation_dispatch = [&](const auto&, std::stop_token) { entered.store(true); return shell::CommandResult{}; };
        for (const std::string code : {"ready\nwhile {1} {catch {while {1} {}}}\n", "ready\nafter 600000\nerror unreachable\n"}) {
            entered.store(false);
            check(runtime.start(files.write("cancel.tcl", code), {}, cancellation_dispatch), "start cancel case");
            check(eventually([&] { return entered.load(); }), "cancel case entered");
            check(!runtime.start(success, {}, dispatch), "second script returns Busy");
            const auto begin = control::Clock::now(); check(runtime.cancel(), "cancel active script");
            check(eventually([&] { return runtime.ready(); }), "cancel completes");
            check(runtime.finish().canceled && control::Clock::now() - begin < 1s, "unwind bypasses catch and wakes after");
        }
        check(run(files.write("exit.tcl", "catch {exit}\nerror unreachable\n")).shutdown, "exit requests framework shutdown and unwinds");
        check(run(files.write("again.tcl", "set x 1\n")).success, "run after cancellation and exit");
        {
            ModuleManager manager; shell::CommandRouter router(manager);
            ModulePolicy policy; policy.stop_timeout = 1s;
            check(bool(manager.register_module({"shellHelper", std::filesystem::absolute(argv[1]), "waitingNode", policy})), "register waiting helper");
            auto name = manager.start_module("shellHelper", {"normal", utf8(files.path)});
            check(name && manager.wait_started(*name), "waiting helper ready");
            auto waiting = files.write("waiting.tcl", "waitingNode slow\nwaitingNode fail\n");
            check(runtime.start(waiting, {}, [&](const auto& args, std::stop_token stop) { return router.execute(args, stop); }), "start waiting script");
            check(eventually([&] { return std::filesystem::exists(files.path / "waitingNode.working"); }), "remote command in progress");
            runtime.cancel(); check(eventually([&] { return runtime.ready(); }), "module wait is cancellable");
            check(runtime.finish().canceled, "module wait cancellation reported");
            auto busy = manager.command(*name, {}, control::Clock::now() + 1s);
            check(!busy && busy.error().code == control::ErrorCode::Backpressure, "cancel does not forget in-flight command");
            check(eventually([&] { return bool(manager.command(*name, {}, control::Clock::now() + 1s)); }), "late response releases Busy");
            check(!std::filesystem::exists(files.path / "waitingNode.failed-command"), "no subsequent script command after cancellation");
            check(manager.inspect(*name)->state == ModuleState::Running, "cancel keeps module running");
            manager.begin_shutdown(); check(bool(manager.wait_shutdown(control::Clock::now() + 3s)), "waiting fixture cleanup");
        }
        {
            ModuleManager manager; std::ostringstream output; StemShell shell(manager, output);
            const auto exe = std::filesystem::absolute(argv[1]);
            check(!manager.register_module({"set", exe, "valid", {}}), "Tcl factory name reserved");
            check(!manager.register_module({"valid", exe, "list", {}}), "Tcl instance base reserved instead of adding suffix");
            ModulePolicy policy; policy.stop_timeout = 1s; policy.startup_timeout = 4s;
            check(bool(manager.register_module({"shellHelper", exe, "node", policy})), "register helper");
            auto startup = files.write("deployment.tcl", "shellHelper normal {" + utf8(files.path) + "}\nnode check {中文 空白} {} {D:\\Some Folder\\a.txt}\nnode fail\nerror unreachable\n");
            InputPipe input; std::atomic<bool> ok{true};
            std::jthread feeder([&] {
                try {
                    check(eventually([&] { return std::filesystem::exists(files.path / "node.failed-command"); }), "startup command executed");
                    std::this_thread::sleep_for(150ms);
                    check(manager.inspect("node")->state == ModuleState::Running, "script error keeps module alive");
                    input.send("help\nexit\n");
                } catch (...) { ok.store(false); input.send("\x03"); }
            });
            shell.run(startup); feeder.join();
            check(ok.load(), "startup feeder");
            check(std::filesystem::exists(files.path / "node.checked"), "real module gets Tcl arguments unchanged");
            check(output.str().find("line 3") != std::string::npos && output.str().find("[Instances]") != std::string::npos, "startup error returns to Shell");
            check(manager.inspect("node")->last_exit.has_value(), "exit waits for module process");
        }
        {
            ModuleManager manager; std::ostringstream output; StemShell shell(manager, output); InputPipe input;
            std::atomic<bool> entered_loop{false}, after_cancel{false};
            check(manager.register_micro_program("loopStarted", [&](auto&) { entered_loop.store(true); return 0; }) == 0, "register loop marker");
            check(manager.register_micro_program("afterCancel", [&](auto&) { after_cancel.store(true); return 0; }) == 0, "register completion marker");
            auto loop = files.write("foreground.tcl", "loopStarted\nwhile {1} {}\n");
            auto second = files.write("second.tcl", "afterCancel\n");
            std::atomic<bool> ok{true};
            std::jthread feeder([&] {
                try {
                    input.send("srun " + shell::quote_argument(utf8(loop)) + "\n");
                    check(eventually([&] { return entered_loop.load(); }), "foreground script running");
                    input.send("afterCancel\n"); std::this_thread::sleep_for(80ms);
                    check(!after_cancel.load(), "foreground script suspends ordinary input");
                    input.send("\x04"); std::this_thread::sleep_for(150ms);
                    input.send("srun " + shell::quote_argument(utf8(second)) + "\n");
                    check(eventually([&] { return after_cancel.load(); }), "Shell accepts script after Ctrl+D");
                    std::this_thread::sleep_for(100ms); input.send("\x04\nexit\n");
                } catch (...) { ok.store(false); input.send("\x03"); }
            });
            shell.run(); feeder.join();
            check(ok.load(), "Ctrl+D foreground feeder");
            check(output.str().find("Script canceled:") != std::string::npos && output.str().find("No script is running") != std::string::npos, "Ctrl+D cancel and idle behavior");
        }
        {
            ModuleManager manager; std::ostringstream output; StemShell shell(manager, output); InputPipe input;
            std::atomic<bool> entered_loop{false};
            check(manager.register_micro_program("loopStarted", [&](auto&) { entered_loop.store(true); return 0; }) == 0, "shutdown loop marker");
            ModulePolicy policy; policy.stop_timeout = 1s;
            check(bool(manager.register_module({"shellHelper", std::filesystem::absolute(argv[1]), "shutdownNode", policy})), "shutdown helper");
            auto startup = files.write("interrupt-startup.tcl", "shellHelper normal {" + utf8(files.path) + "}\nloopStarted\nwhile {1} {}\n");
            std::atomic<bool> ok{true};
            std::jthread feeder([&] {
                if (!eventually([&] { return entered_loop.load(); })) ok.store(false);
                input.send("\x03");
            });
            shell.run(startup); feeder.join();
            check(ok.load(), "startup loop reached");
            check(manager.inspect("shutdownNode")->last_exit.has_value() && std::filesystem::exists(files.path / "shutdownNode.stopped"), "Ctrl+C during startup waits for module cleanup");
        }
        std::cout << "Tcl integration tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
