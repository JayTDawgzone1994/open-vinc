#include <module/module_manager.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <random>
#include <stdexcept>
#include <thread>
using namespace vinc;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = control::Clock::now() + timeout;
    do { if (predicate()) return true; std::this_thread::sleep_for(20ms); } while (control::Clock::now() < deadline);
    return predicate();
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const auto helper = std::filesystem::absolute(argv[1]);
        const auto silent_helper = std::filesystem::absolute(argv[2]);
        ModuleManager manager;
        ModulePolicy policy; policy.heartbeat_interval = 80ms; policy.heartbeat_timeout = 800ms;
        policy.startup_timeout = 1500ms; policy.stop_timeout = 300ms; policy.restart_delay = 150ms;
        check(bool(manager.register_module({"helper", helper, "node", policy})), "register");
        check(!manager.register_module({"helper", helper, "node", policy}), "duplicate factory rejected");
        std::vector<std::future<control::Result<std::string>>> starts;
        for (int i = 0; i < 4; ++i) starts.push_back(std::async(std::launch::async, [&] { return manager.start_module("helper"); }));
        std::set<std::string> names;
        for (auto& future : starts) { auto result = future.get(); check(bool(result), "concurrent start"); names.insert(*result); }
        check(names == std::set<std::string>{"node", "node_1", "node_2", "node_3"}, "unique suffix allocation");
        std::set<std::uint64_t> pids;
        for (const auto& name : names) {
            auto ready = manager.wait_running(name, control::Clock::now() + 8s);
            if (!ready) std::cerr << name << ": " << ready.error().message << '\n';
            check(bool(ready), "module readiness");
            pids.insert(manager.inspect(name)->pid);
            auto command = manager.command(name, {"name", name}, control::Clock::now() + 2s);
            check(command && *command == 0, "central name delivered to virtual hook");
        }
        check(pids.size() == names.size(), "one process per instance");
        auto unicode = manager.command("node", {"unicode", "模組測試", "a b\"c", ""}, control::Clock::now() + 2s);
        check(unicode && *unicode == 0, "UTF-8 command parameters");
        check(manager.call_node_autoComplete("node", {}) == std::vector<std::string>{"hello", "status"}, "virtual completion");
        std::this_thread::sleep_for(1700ms);
        check(manager.inspect("node")->state == ModuleState::Running, "heartbeats while idle");
        auto thrown = manager.command("node", {"throw"}, control::Clock::now() + 2s);
        check(thrown && *thrown == -111, "hook exception reply");
        (void)manager.command("node", {"crash"}, control::Clock::now() + 2s);
        check(eventually([&] { return manager.inspect("node")->state == ModuleState::Failed; }), "crash observed");
        auto survivor = manager.command("node_1", {}, control::Clock::now() + 2s);
        check(survivor && *survivor == 42, "other module survives crash");
        auto hang = manager.command("node_1", {"hang"}, control::Clock::now() + 100ms);
        check(!hang && hang.error().code == control::ErrorCode::Timeout, "command timeout");
        std::this_thread::sleep_for(1000ms);
        check(manager.inspect("node_1")->state == ModuleState::Running, "hook hang does not block heartbeat");
        check(bool(manager.stop_module("node_1", control::Clock::now() + 3s)), "hung hook forcibly stopped");
        for (const auto& name : names) check(bool(manager.stop_module(name, control::Clock::now() + 3s)), "stop/reap");
        auto reused = manager.start_module("helper"); check(reused && *reused == "node", "reuse final name after exit");
        check(bool(manager.wait_running(*reused, control::Clock::now() + 5s)), "repeat start");
        check(bool(manager.stop_module(*reused, control::Clock::now() + 3s)), "repeat stop");
        auto blocked = manager.start_module("helper", {"init-hang"}); check(bool(blocked), "start hung initialization");
        check(eventually([&] { return manager.inspect(*blocked)->state == ModuleState::Failed; }), "readiness timeout terminates process");
        auto bad = manager.start_module("helper", {"fail"}); check(bool(bad), "start rejected initialization");
        check(!manager.wait_running(*bad, control::Clock::now() + 5s), "startup failure surfaced");
        check(bool(manager.unregister_module_factory("helper") == 0), "unregister factory");
        policy.max_restarts = 2;
        check(bool(manager.register_module({"helper", helper, "retry", policy})), "register retry policy");
        auto retry = manager.start_module("helper", {"crash-start"}); check(bool(retry), "retry start");
        check(eventually([&] { return manager.inspect(*retry)->state == ModuleState::Failed; }, 10s), "restart budget exhausted");
        check(manager.inspect(*retry)->restarts == 2, "exact retry limit");
        check(manager.inspect(*retry)->last_exit && manager.inspect(*retry)->last_exit->value == 42, "native exit code retained");
        check(bool(manager.forget_module(*retry)), "forget terminal record");
        auto recover = manager.start_module("helper"); check(bool(recover), "start recoverable module");
        check(bool(manager.wait_running(*recover, control::Clock::now() + 4s)), "recoverable ready");
        const auto original = *manager.inspect(*recover);
        (void)manager.command(*recover, {"crash"}, control::Clock::now() + 2s);
        check(eventually([&] { auto state = *manager.inspect(*recover); return state.state == ModuleState::Running && state.generation != original.generation; }), "running crash restarts into new session");
        check(manager.inspect(*recover)->restarts == 1 && manager.inspect(*recover)->name == original.name, "restart retains logical name");
        check(bool(manager.command(*recover, {}, control::Clock::now() + 2s)), "new session command");
        check(bool(manager.stop_module(*recover, control::Clock::now() + 3s)), "explicit stop after restart");
        std::this_thread::sleep_for(400ms);
        check(manager.inspect(*recover)->state == ModuleState::Stopped && manager.inspect(*recover)->restarts == 1, "explicit stop never restarts");
        check(bool(manager.unregister_module_factory("helper") == 0), "unregister retry factory");
        check(bool(manager.register_module({"helper", helper.parent_path() / "does-not-exist.exe", "missing", policy})), "register missing executable");
        auto missing = manager.start_module("helper"); check(bool(missing), "queue missing executable");
        check(eventually([&] { return manager.inspect(*missing)->state == ModuleState::Failed; }), "spawn failure bounded");
        check(manager.inspect(*missing)->os_error.has_value(), "native launch error retained");
        check(bool(manager.unregister_module_factory("helper") == 0), "unregister missing factory");
        policy.max_restarts = 0;
        check(bool(manager.register_module({"silent", silent_helper, "silentNode", policy})), "register silent peer");
        auto silent = manager.start_module("silent"); check(bool(silent), "start silent peer");
        check(bool(manager.wait_running(*silent, control::Clock::now() + 3s)), "silent peer handshake");
        check(eventually([&] { return manager.inspect(*silent)->state == ModuleState::Failed; }), "central kills silent module");
        check(manager.inspect(*silent)->last_error == "heartbeat timeout" && manager.inspect(*silent)->last_exit.has_value(), "heartbeat cause and reaped exit retained");

        check(bool(manager.register_module({"helper", helper, "graceful", policy})), "register graceful helper");
        const auto marker = std::filesystem::temp_directory_path() / ("ov-stop-" + std::to_string(std::random_device{}()) + ".txt");
        const auto marker_utf8 = marker.u8string();
        auto graceful = manager.start_module("helper", {"marker", std::string(reinterpret_cast<const char*>(marker_utf8.data()), marker_utf8.size())});
        check(graceful && manager.wait_running(*graceful, control::Clock::now() + 3s), "graceful ready");
        check(bool(manager.stop_module(*graceful, control::Clock::now() + 3s)), "graceful stop");
        check(manager.inspect(*graceful)->last_exit && manager.inspect(*graceful)->last_exit->value == 0, "normal exit code");
        std::ifstream marker_file(marker); std::string stopped_name; marker_file >> stopped_name; marker_file.close();
        std::filesystem::remove(marker); check(stopped_name == *graceful, "on_stop executed");
        {
            ModuleManager scoped;
            check(bool(scoped.register_module({"helper", helper, "scopeNode", policy})), "scoped registration");
            auto name = scoped.start_module("helper", {"marker", std::string(reinterpret_cast<const char*>(marker_utf8.data()), marker_utf8.size())});
            check(name && scoped.wait_running(*name, control::Clock::now() + 3s), "scoped module ready");
        }
        std::ifstream shutdown_file(marker); std::string shutdown_name; shutdown_file >> shutdown_name; shutdown_file.close();
        std::filesystem::remove(marker); check(shutdown_name == "scopeNode", "manager destruction stops child gracefully");

        // Own the real module child directly to verify self-exit when its central
        // stops heartbeats without closing the socket, and when the socket closes.
        auto process_service = osal::ProcessService::create(); check(bool(process_service), "orphan test process service");
        struct EndpointCleanup {
            std::filesystem::path path;
            ~EndpointCleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
        } endpoint{std::filesystem::temp_directory_path() / ("ov-hb-" + std::to_string(std::random_device{}()) + ".sock")};
        const auto endpoint_u8 = endpoint.path.u8string();
        const std::string endpoint_text(reinterpret_cast<const char*>(endpoint_u8.data()), endpoint_u8.size());
        auto listener = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
        check(listener && listener->bind(osal::LocalEndpoint{endpoint_text}) && listener->listen(4), "raw central listener");
        for (bool disconnect : {false, true}) {
            osal::SpawnOptions spawn; spawn.executable = helper;
            spawn.arguments = {"--vinc-module-v1", endpoint_text, "orphan", "orphan-test", "1234", "40", "400", "2000", "200", "helper"};
            auto child = process_service->spawn(spawn); check(bool(child), "spawn orphan test");
            struct ChildCleanup {
                osal::Process& child; osal::ProcessService& service;
                ~ChildCleanup() { (void)child.terminate(); (void)child.wait_until(control::Clock::now() + 2s); (void)service.forget(child.id()); }
            } cleanup{*child, *process_service};
            auto socket = listener->accept_until(control::Clock::now() + 3s); check(bool(socket), "accept orphan");
            control::Frame hello{control::Kind::Hello};
            control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Central));
            control::put_u64(hello.payload, 1234); control::put_string(hello.payload, "orphan-test");
            control::put_u32(hello.payload, 40); control::put_u32(hello.payload, 400);
            auto bytes = control::encode(hello); std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto sent = socket->send_until(std::span<const std::byte>(bytes).subspan(offset), control::Clock::now() + 2s);
                check(sent && *sent, "send central handshake"); offset += *sent;
            }
            // Wait for Ready so the test distinguishes watchdog exit from failed startup.
            control::Decoder decoder; bool ready = false; std::array<std::byte, 1024> buffer;
            while (!ready) {
                auto got = socket->receive_until(buffer, control::Clock::now() + 2s);
                check(got && !got->eof, "receive module ready");
                auto frames = decoder.feed(std::span<const std::byte>(buffer).first(got->bytes)); check(bool(frames), "decode ready");
                for (const auto& f : *frames) if (f.kind == control::Kind::Event && f.domain == 1 && f.operation == 1) ready = true;
            }
            if (disconnect) check(bool(socket->close()), "close central socket");
            auto exit = child->wait_until(control::Clock::now() + 3s);
            check(exit && exit->value == 72, disconnect ? "module exits when central disconnects" : "module exits when central heartbeat stops");
        }
        std::cout << "module process lifecycle, naming, commands, isolation, timeout and restart tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
