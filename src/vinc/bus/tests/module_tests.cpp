// End-to-end Bus tests through independent Module processes and the central broker.
// Verify multiple Publishers, copied reads, loans, topology changes and crash cleanup.

#include <module/module_manager.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>
using namespace vinc;
using namespace std::chrono_literals;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        ModuleManager manager;
        ModulePolicy policy; policy.heartbeat_interval = 100ms; policy.heartbeat_timeout = 1500ms; policy.startup_timeout = 15s;
        check(bool(manager.register_module({"busTest", std::filesystem::absolute(argv[1]), "busNode", policy})), "register helper");
        auto start = [&](std::vector<std::string> args) {
            auto name = manager.start_module("busTest", std::move(args)); check(bool(name), "start helper");
            auto r = manager.wait_running(*name, control::Clock::now() + 18s);
            if (!r) { const auto s = manager.inspect(*name); throw std::runtime_error(r.error().message + " " + s->last_error); }
            return *name;
        };
        auto command = [&](const std::string& node, std::vector<std::string> args) {
            auto r = manager.command(node, std::move(args), control::Clock::now() + 10s);
            if (!r) throw std::runtime_error(r.error().message); return *r;
        };
        auto eventually = [&](auto predicate) {
            const auto end = control::Clock::now() + 5s;
            while (control::Clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(10ms); }
            return predicate();
        };
        auto sub = start({"sub", "frames"});
        check(command(sub, {"last"}) == -102, "get_last before Publisher fails immediately");
        check(command(sub, {"next"}) == -101, "next before Publisher fails immediately");
        auto a = start({"pub", "frames", "1", "4194304"});
        check(command(a, {"slots"}) == 2, "one requested slot becomes two");
        check(command(a, {"wrong-schema"}) == -107, "topic schema mismatch rejected");
        check(command(a, {"push", "1"}) == 0, "local push");
        check(eventually([&] { return command(sub, {"last"}) == 1; }), "first shared message");
        check(command(sub, {"last"}) == 1, "latest repeats");
        check(command(sub, {"next"}) == 1, "latest did not advance next");
        check(command(sub, {"next"}) == -101, "no new message");
        check(command(a, {"loan", "2", "2097152"}) == 0, "shared loan with large payload");
        check(command(sub, {"last"}) == 1, "last remains visible during loan");
        check(command(a, {"loan", "99"}) == -103, "second loan fails");
        check(command(a, {"push", "99"}) == -103, "push cannot bypass outstanding loan");
        check(command(a, {"commit"}) == 0, "commit shared message");
        check(command(sub, {"next"}) == 2, "large nested message received");
        check(command(sub, {"copy-size"}) == 2097152, "full payload copied");
        auto old = start({"sub", "frames"});
        check(command(old, {"next"}) == 1, "default subscription starts oldest retained");
        auto fresh = start({"sub", "frames", "new"});
        check(command(fresh, {"next"}) == -101, "new-only cursor starts at subscription");
        check(command(fresh, {"last"}) == 2, "latest is independent of new-only cursor");
        auto b = start({"pub", "frames", "3", "8192"});
        check(command(b, {"push", "3"}) == 0, "second Publisher");
        check(eventually([&] { return command(sub, {"last"}) == 3; }), "dynamic Publisher attachment");
        check(command(a, {"push", "4"}) == 0, "publish after second Publisher");
        check(command(sub, {"next"}) == 3 && command(sub, {"next"}) == 4, "merge visible candidates by timestamp");
        check(command(b, {"push", "5", "9000"}) == -106, "oversize fails");
        check(command(b, {"published"}) == 1 && command(sub, {"last"}) == 4, "failed push preserves publication history");
        check(command(a, {"push", "5"}) == 0 && command(a, {"push", "6"}) == 0, "overwrite oldest history");
        check(command(old, {"next"}) == 3 && command(old, {"next"}) == 5 && command(old, {"next"}) == 6, "slow next skips only overwritten entries");
        check(command(a, {"loan", "7"}) == 0 && command(a, {"drop"}) == 0, "discard outstanding object");
        check(command(a, {"loan", "8"}) == -103, "dropped loan remains outstanding");
        check(command(sub, {"last"}) == 6, "abandoned loan leaves last published message");
        (void)manager.command(a, {"crash"}, control::Clock::now() + 2s);
        check(eventually([&] { return manager.inspect(a)->state == ModuleState::Failed; }), "Publisher death detected");
        check(eventually([&] { return command(sub, {"last"}) == 3; }), "dead Publisher history retired");
        check(command(old, {"copy-value"}) == 6, "received copy survives Publisher death");
        check(command(b, {"push", "9"}) == 0, "other Publisher survives");
        check(command(sub, {"next"}) == 9, "subscription survives Publisher death");
        check(command(b, {"close"}) == 0, "normal Publisher close");
        check(eventually([&] { return command(sub, {"last"}) == -102; }), "last Publisher removal gives no data");
        auto restarted = start({"pub", "frames", "2", "8192"});
        check(command(restarted, {"push", "10"}) == 0, "new Publisher instance");
        check(eventually([&] { return command(sub, {"next"}) == 10; }), "new instance has independent cursor");
        auto pinned_reader = start({"sub", "fault"});
        auto blocked_writer = start({"pub", "fault", "2", "8192"});
        check(command(blocked_writer, {"push", "20"}) == 0, "fault setup first message");
        check(eventually([&] { return command(pinned_reader, {"last"}) == 20; }), "reader attachment before pin");
        check(command(pinned_reader, {"pin"}) == 0, "hold reader at copy boundary");
        check(command(blocked_writer, {"push", "21"}) == 0, "other slot remains writable");
        auto waiting = std::async(std::launch::async, [&] { return command(blocked_writer, {"push", "22"}); });
        check(waiting.wait_for(1700ms) == std::future_status::timeout, "collision waits past heartbeat timeout");
        check(manager.inspect(blocked_writer)->state == ModuleState::Running && manager.inspect(pinned_reader)->state == ModuleState::Running, "heartbeats survive blocked writer and pinned reader");
        (void)manager.command(pinned_reader, {"crash"}, control::Clock::now() + 2s);
        check(waiting.get() == 0, "confirmed reader death releases writer");
        check(command(blocked_writer, {"collisions"}) == 1 && command(blocked_writer, {"waited"}) == 1, "collision count and duration recorded");
        check(bool(manager.stop_module(blocked_writer, control::Clock::now() + 5s)), "stop recovered writer");
        for (const auto& node : {sub, old, fresh, b, restarted}) check(bool(manager.stop_module(node, control::Clock::now() + 5s)), "stop helper");
        std::cout << "bus module integration passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
