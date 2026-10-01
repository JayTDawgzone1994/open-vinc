#include "input_pipe.hpp"
#include <stem_shell/stem_shell.hpp>
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
    check(bool(manager.register_module({"shellHelper", exe, "node", policy})), "register helper");
}
std::string start(ModuleManager& manager, const Workspace& files, const std::string& mode) {
    auto name = manager.start_module("shellHelper", {mode, utf8(files.path)}); check(bool(name), "start helper");
    check(bool(manager.wait_started(*name)), "wait helper ready"); return *name;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const auto exe = std::filesystem::absolute(argv[1]);
        {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto node = start(manager, files, "slow-stop");
            shell::CommandRouter router(manager);
            auto parsed = shell::parse_line(node + R"( check "中文 空白" "" "D:\Some Folder\a.txt")");
            check(parsed && router.execute(parsed->arguments).success && std::filesystem::exists(files.path / (node + ".checked")), "quoted arguments arrive unchanged at remote hook");
            check(router.complete(node + " ").choices == std::vector<std::string>{"alpha", "beta"}, "remote parameter completion");
            auto timed = manager.command(node, {"slow"}, control::Clock::now() + 30ms);
            check(!timed && timed.error().code == control::ErrorCode::Timeout, "slow command timeout");
            auto busy = manager.command(node, {}, control::Clock::now() + 1s);
            check(!busy && busy.error().code == control::ErrorCode::Backpressure, "timed out request is still in flight");
            check(eventually([&] { auto r = manager.command(node, {}, control::Clock::now() + 1s); return r && *r == 0; }), "late reply releases busy state");
            manager.begin_shutdown();
            check(!manager.start_module("shellHelper"), "shutdown rejects starts");
            auto pending = manager.wait_shutdown(control::Clock::now() + 20ms);
            check(!pending && pending.error().code == control::ErrorCode::Timeout, "shutdown timeout keeps supervision");
            check(bool(manager.wait_shutdown(control::Clock::now() + 4s)), "shutdown waits for actual exit");
            check(std::filesystem::exists(files.path / (node + ".stopped")), "on_stop completed before shutdown success");
            check(manager.inspect(node)->last_exit.has_value(), "OS exit observed");
        }
        {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto normal = start(manager, files, "slow-stop");
            const auto hung = start(manager, files, "normal");
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
            check(text.find("Busy:") != std::string::npos && text.find("[Instances]") != std::string::npos, "busy reporting and help stay responsive");
            check(text.find("All module processes exited; Bus cleanup complete.") != std::string::npos, "shutdown completion displayed after confirmation");
        }
        for (const bool eof : {false, true}) {
            Workspace files; ModuleManager manager; register_helper(manager, exe);
            const auto node = start(manager, files, "normal");
            InputPipe input; std::ostringstream output; StemShell shell(manager, output);
            if (eof) input.end(); else input.send("exit\n");
            shell.run();
            check(std::filesystem::exists(files.path / (node + ".stopped")) && manager.inspect(node)->last_exit.has_value(), "exit and EOF use the same full shutdown");
        }
        std::cout << "shell integration tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
