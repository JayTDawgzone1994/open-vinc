// Run Shell against real module helpers while feeding controlled terminal input.
// Verify responsive commands, late replies and shutdown that waits for actual child exit.

#include "input_pipe.hpp"
#include <stem_shell/stem_shell.hpp>
#include <stem_shell/tcl_runtime.hpp>
#include <stem_shell/command_router.hpp>
#include <atomic>
#include <filesystem>
#include <future>
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
std::string utf8(const std::filesystem::path& path) { const auto text = path.u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()}; }
struct Workspace {
    const std::filesystem::path root = std::filesystem::absolute(std::filesystem::current_path());
    std::filesystem::path path = root / ("shell-test-" + std::to_string(control::Clock::now().time_since_epoch().count()));
    Workspace() { check(std::filesystem::create_directory(path), "create test directory"); }
    ~Workspace() { if (path.parent_path() == root) { std::error_code e; std::filesystem::remove_all(path, e); } }
};
void register_helper(ModuleManager& manager, const std::filesystem::path& exe) {
    ModulePolicy policy; policy.heartbeat_interval = 80ms; policy.heartbeat_timeout = 1200ms;
    policy.startup_timeout = 5s; policy.stop_timeout = 1500ms;
    check(bool(manager.register_module_type({"shellHelper", exe, "node", policy})), "register helper");
}
std::string start(ModuleManager& manager, const Workspace& files, const std::string& mode, const std::string& module_name = "node") {
    auto name = manager.start_module("shellHelper", {"--name", module_name, mode, utf8(files.path)}); check(bool(name), "start helper");
    check(bool(manager.wait_started(*name)), "wait helper ready"); return manager.inspect(*name)->name;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        shell::initialize_tcl(argv[0]);
        const auto exe = std::filesystem::absolute(argv[1]);
        {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            InputPipe input; std::ostringstream output; StemShell shell(manager, output);
            const auto module = start(manager, files, "output");
            std::atomic<bool> feeder_ok{true};
            std::jthread feeder([&] {
                try {
                    input.send(module + " output\n");
                    check(eventually([&] { return std::filesystem::exists(files.path / (module + ".output")); }), "output command executed");
                    // Leave an incomplete command while module output arrives.
                    input.send("hel"); std::this_thread::sleep_for(150ms); input.send("p\nexit\n");
                } catch (...) { feeder_ok = false; input.end(); }
            });
            shell.run(); feeder.join(); check(feeder_ok, "output feeder");
            const auto text = output.str();
            check(text.find("startup-output") != std::string::npos && text.find("startup-error") != std::string::npos,
                "startup stdout/stderr not routed to Shell");
            check(text.find("command-output\\x1B[2J") != std::string::npos && text.find('\x1b') == std::string::npos,
                "module ANSI was executed or output lost");
            check(text.find("[Modules]") != std::string::npos, "module output corrupted incomplete command");
            const auto tail = text.find("stop-tail-without-newline");
            check(tail != std::string::npos && tail < text.find("All module processes exited;"), "shutdown lost or reordered output tail");
            check(manager.output_hub().stats().active_captures == 0, "shutdown retained captures");
        }
        {
            Workspace files; ModuleManager manager;
            ModulePolicy policy; policy.heartbeat_interval = 80ms; policy.heartbeat_timeout = 1200ms;
            policy.max_restarts = 1; policy.restart_delay = 100ms;
            check(bool(manager.register_module_type({"shellHelper", exe, "node", policy})), "register restart helper");
            auto destination = manager.output_hub().subscribe(); check(bool(destination), "restart output subscriber");
            const auto module = start(manager, files, "output");
            const auto first = manager.inspect(module)->generation;
            (void)manager.command(module, {"crash-output"}, control::Clock::now() + 2s);
            check(eventually([&] { auto snapshot = manager.inspect(module); return snapshot && snapshot->generation != first &&
                snapshot->state == ModuleState::Running; }), "restart became ready");
            const auto second = manager.inspect(module)->generation;
            check(bool(manager.wait_shutdown(control::Clock::now() + 5s)), "restart output shutdown");
            auto batch = destination->take(128);
            std::string first_bytes, second_bytes;
            for (const auto& event : batch.events) {
                if (event->source->stream != output::Stream::Stdout) continue;
                if (event->source->generation == first) first_bytes += event->bytes;
                if (event->source->generation == second) second_bytes += event->bytes;
            }
            check(first_bytes.find("startup-output") != std::string::npos && first_bytes.find("crash-output") != std::string::npos &&
                second_bytes.find("startup-output") != std::string::npos, "restart output generation association");
        }
        {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto module = start(manager, files, "slow-stop");
            shell::CommandRouter router(manager);
            auto parsed = shell::parse_line(module + R"( check "中文 空白" "" "D:\Some Folder\a.txt")");
            check(parsed && router.execute(parsed->arguments).success && std::filesystem::exists(files.path / (module + ".checked")), "quoted arguments arrive unchanged at remote hook");
            check(router.complete(module + " ").choices == std::vector<std::string>{"alpha", "beta"}, "remote parameter completion");
            auto timed = manager.command(module, {"slow"}, control::Clock::now() + 30ms);
            check(!timed && timed.error().code == control::ErrorCode::Timeout, "slow command timeout");
            auto busy = manager.command(module, {}, control::Clock::now() + 1s);
            check(!busy && busy.error().code == control::ErrorCode::Backpressure, "timed out request is still in flight");
            check(eventually([&] { auto r = manager.command(module, {}, control::Clock::now() + 1s); return r && *r == 0; }), "late reply releases busy state");
            manager.begin_shutdown();
            check(!manager.start_module("shellHelper"), "shutdown rejects starts");
            auto pending = manager.wait_shutdown(control::Clock::now() + 20ms);
            check(!pending && pending.error().code == control::ErrorCode::Timeout, "shutdown timeout keeps supervision");
            check(bool(manager.wait_shutdown(control::Clock::now() + 4s)), "shutdown waits for actual exit");
            check(std::filesystem::exists(files.path / (module + ".stopped")), "on_stop completed before shutdown success");
            check(manager.inspect(module)->last_exit.has_value(), "OS exit observed");
        }
        {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto normal = start(manager, files, "slow-stop");
            const auto hung = start(manager, files, "normal", "hungNode");
            InputPipe input; std::ostringstream output; StemShell shell(manager, output);
            std::atomic<bool> feeder_ok{true};
            control::Deadline interrupted_at;
            std::jthread feeder([&] {
                try {
                    input.send("unknown-command\n" + hung + " hang\n");
                    check(eventually([&] { return std::filesystem::exists(files.path / (hung + ".working")); }), "hang command entered");
                    input.send(hung + " fail\nhelp\n" + normal + " fail\n");
                    check(eventually([&] { return std::filesystem::exists(files.path / (normal + ".failed-command")); }), "other module command executed while first is hung");
                    std::this_thread::sleep_for(100ms);
                    interrupted_at = control::Clock::now();
                    input.send("\x03\x03\x03");
                } catch (...) { feeder_ok.store(false); input.end(); }
            });
            shell.run(); feeder.join();
            check(feeder_ok.load(), "input feeder");
            check(control::Clock::now() - interrupted_at >= 450ms, "Ctrl+C waits for slow normal stop");
            check(control::Clock::now() - interrupted_at < 5s, "Ctrl+C responsive while command is hung");
            check(std::filesystem::exists(files.path / (normal + ".stopped")), "normal process finished cleanup");
            check(!std::filesystem::exists(files.path / (hung + ".stopped")), "hung hook required forced termination");
            for (const auto& s : manager.snapshots()) check(s.state == ModuleState::Stopped && s.last_exit.has_value(), "all process exits confirmed");
            const auto text = output.str();
            check(text.find("Unknown command") != std::string::npos && text.find("returned 17") != std::string::npos, "interactive failures are displayed");
            check(text.find("Busy:") != std::string::npos && text.find("[Modules]") != std::string::npos, "busy reporting and help stay responsive");
            check(text.find("All module processes exited; Bus cleanup complete.") != std::string::npos, "shutdown completion displayed after confirmation");
        }
        for (const bool eof : {false, true}) {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto module = start(manager, files, "normal");
            InputPipe input; std::ostringstream output; StemShell shell(manager, output);
            if (eof) input.end(); else input.send("exit\n");
            shell.run();
            check(std::filesystem::exists(files.path / (module + ".stopped")) && manager.inspect(module)->last_exit.has_value(), "exit and EOF use the same full shutdown");
        }
        std::cout << "shell integration tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
