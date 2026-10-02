// Central supervision integration tests against real helper processes.
// Check readiness, exclusive name registration, request failure, parent loss, restart and stop behavior.

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
        check(bool(manager.register_module_type({"helper", helper, "node", policy})), "register");
        check(!manager.register_module_type({"helper", helper, "node", policy}), "duplicate module type rejected");
        // The legacy adapters distinguish starting a ModuleType from commanding a Module.
        check(manager.has_module_type("helper") && !manager.has_active_module("helper"), "type registration does not start a module");
        check(manager.get_registered_module_type_names() == std::vector<std::string>{"helper"}, "registered module types");
        check(manager.call_module_type_interface("helper", {"--name", "adapterModule"}) == 0, "type adapter starts and waits");
        check(manager.has_active_module("adapterModule") &&
            manager.get_active_module_names() == std::vector<std::string>{"adapterModule"}, "active modules use final names");
        check(manager.call_module_interface("adapterModule", {"name", "adapterModule"}) == 0, "module adapter sends command to existing process");
        check(manager.call_module_type_autoComplete("helper", {}).empty(), "type completion remains empty");
        check(manager.call_module_autoComplete("adapterModule", {}) == std::vector<std::string>{"hello", "status"}, "module adapter invokes completion hook");
        check(bool(manager.stop_module("adapterModule", control::Clock::now() + 3s)), "stop adapter module");
        check(bool(manager.forget_module("adapterModule")), "forget adapter module");
        std::vector<std::future<control::Result<MID>>> starts;
        for (const auto& name : {"node", "workerA", "workerB", "workerC"})
            starts.push_back(std::async(std::launch::async, [&, name] { return manager.start_module("helper", {"--name", name}); }));
        std::set<std::string> names;
        std::set<MID> mids;
        for (auto& future : starts) {
            auto result = future.get(); check(bool(result), "concurrent start"); mids.insert(*result);
            check(bool(manager.wait_started(*result)), "named startup"); names.insert(manager.inspect(*result)->name);
            const auto snapshot = manager.inspect(*result);
            check(snapshot->mid == *result && snapshot->module_type == "helper" && snapshot->generation == snapshot->mid,
                "snapshot exposes MID and ModuleType while preserving runtime generation");
            check(manager.command(*result, {"mid", std::to_string(*result)}, control::Clock::now() + 2s).value_or(-1) == 0, "central MID delivered to Module");
        }
        check(names == std::set<std::string>{"node", "workerA", "workerB", "workerC"} && mids.size() == 4, "explicit names and unique MIDs");
        auto duplicate = manager.start_module("helper"); check(bool(duplicate), "duplicate gets supervised startup MID");
        auto duplicate_ready = manager.wait_started(*duplicate);
        check(!duplicate_ready && duplicate_ready.error().message.find("already exists: node") != std::string::npos, "duplicate name rejected clearly");
        check(!manager.inspect("node_1"), "no automatic suffix created");
        check(eventually([&] { return manager.inspect(*duplicate)->last_exit.has_value(); }), "rejected child reaped");
        check(bool(manager.forget_module(*duplicate)), "forget rejected startup by MID");
        // Both children may propose the same name before either has reached Ready.
        auto claim_a = manager.start_module("helper", {"--name", "contended"});
        auto claim_b = manager.start_module("helper", {"--name", "contended"});
        check(claim_a && claim_b && *claim_a != *claim_b, "contending starts have distinct identities");
        const bool ready_a = bool(manager.wait_started(*claim_a));
        const bool ready_b = bool(manager.wait_started(*claim_b));
        check(ready_a != ready_b, "only one concurrent name claim succeeds");
        check(bool(manager.stop_module(*claim_a, control::Clock::now() + 3s)) &&
            bool(manager.stop_module(*claim_b, control::Clock::now() + 3s)), "reap both name contenders");
        check(bool(manager.reserve_command_name("reservedProbe")), "reserve command for naming test");
        const auto start_marker = std::filesystem::temp_directory_path() / ("ov-start-" + std::to_string(std::random_device{}()) + ".txt");
        const auto start_marker_u8 = start_marker.u8string();
        const std::string start_marker_text(reinterpret_cast<const char*>(start_marker_u8.data()), start_marker_u8.size());
        for (const std::string proposed : {"node", "reservedProbe", "helper", "", "bad\nname"}) {
            auto rejected = manager.start_module("helper", {"--name", proposed, "start-marker", start_marker_text});
            check(rejected && !manager.wait_started(*rejected), "reject duplicate, reserved or invalid proposed name");
            check(bool(manager.stop_module(*rejected, control::Clock::now() + 3s)), "reap invalid name child");
            const bool started = std::filesystem::exists(start_marker);
            std::filesystem::remove(start_marker);
            check(!started, "rejected name never reaches on_start");
        }
        auto unnamed = manager.start_module("helper", {"name-hang"});
        check(unnamed && eventually([&] { auto s = manager.inspect(*unnamed); return s && s->pid != 0; }), "unnamed child supervised by MID");
        check(manager.inspect(*unnamed)->name.empty(), "name unclaimed during naming hook");
        check(bool(manager.stop_module(*unnamed, control::Clock::now() + 3s)), "stop hung naming hook by MID");
        for (const auto* mode : {"name-hang", "name-throw", "name-crash"}) {
            auto failed_name = manager.start_module("helper", {mode});
            check(failed_name && !manager.wait_started(*failed_name), "naming hook failure is bounded");
            check(eventually([&] { auto s = manager.inspect(*failed_name); return s && s->state == ModuleState::Failed && s->last_exit.has_value(); }), "failed naming child reaped");
        }
        std::set<std::uint64_t> pids;
        for (const auto& name : names) {
            auto ready = manager.wait_running(name, control::Clock::now() + 8s);
            if (!ready) std::cerr << name << ": " << ready.error().message << '\n';
            check(bool(ready), "module readiness");
            pids.insert(manager.inspect(name)->pid);
            auto command = manager.command(name, {"name", name}, control::Clock::now() + 2s);
            check(command && *command == 0, "central name delivered to virtual hook");
        }
        check(pids.size() == names.size(), "one process per Module");
        auto unicode = manager.command("node", {"unicode", "模組測試", "a b\"c", ""}, control::Clock::now() + 2s);
        check(unicode && *unicode == 0, "UTF-8 command parameters");
        check(manager.call_module_autoComplete("node", {}) == std::vector<std::string>{"hello", "status"}, "virtual completion");
        std::this_thread::sleep_for(1700ms);
        check(manager.inspect("node")->state == ModuleState::Running, "heartbeats while idle");
        auto thrown = manager.command("node", {"throw"}, control::Clock::now() + 2s);
        check(thrown && *thrown == -111, "hook exception reply");
        (void)manager.command("node", {"crash"}, control::Clock::now() + 2s);
        check(eventually([&] { return manager.inspect("node")->state == ModuleState::Failed; }), "crash observed");
        auto survivor = manager.command("workerA", {}, control::Clock::now() + 2s);
        check(survivor && *survivor == 42, "other module survives crash");
        auto hang = manager.command("workerA", {"hang"}, control::Clock::now() + 100ms);
        check(!hang && hang.error().code == control::ErrorCode::Timeout, "command timeout");
        std::this_thread::sleep_for(1000ms);
        check(manager.inspect("workerA")->state == ModuleState::Running, "hook hang does not block heartbeat");
        check(bool(manager.stop_module("workerA", control::Clock::now() + 3s)), "hung hook forcibly stopped");
        for (const auto& name : names) check(bool(manager.stop_module(name, control::Clock::now() + 3s)), "stop/reap");
        auto reused = manager.start_module("helper"); check(reused && !mids.contains(*reused), "new MID after exit");
        check(bool(manager.wait_running(*reused, control::Clock::now() + 5s)), "repeat start");
        check(manager.inspect(*reused)->name == "node", "reuse final name after exit");
        check(bool(manager.stop_module(*reused, control::Clock::now() + 3s)), "repeat stop");
        auto blocked = manager.start_module("helper", {"init-hang"}); check(bool(blocked), "start hung initialization");
        check(eventually([&] { return manager.inspect(*blocked)->state == ModuleState::Failed; }), "readiness timeout terminates process");
        auto bad = manager.start_module("helper", {"fail"}); check(bool(bad), "start rejected initialization");
        check(!manager.wait_running(*bad, control::Clock::now() + 5s), "startup failure surfaced");
        check(bool(manager.unregister_module_type("helper") == 0), "unregister module type");
        policy.max_restarts = 2;
        check(bool(manager.register_module_type({"helper", helper, "retry", policy})), "register retry policy");
        auto retry = manager.start_module("helper", {"crash-start"}); check(bool(retry), "retry start");
        check(eventually([&] { auto s = manager.inspect("retry"); return s && s->state == ModuleState::Failed; }, 10s), "restart budget exhausted");
        check(manager.inspect("retry")->restarts == 2 && !manager.inspect(*retry), "exact retry limit and retired first MID");
        check(manager.inspect("retry")->last_exit && manager.inspect("retry")->last_exit->value == 42, "native exit code retained");
        check(bool(manager.forget_module("retry")), "forget terminal record");
        auto recover = manager.start_module("helper"); check(bool(recover), "start recoverable module");
        check(bool(manager.wait_running(*recover, control::Clock::now() + 4s)), "recoverable ready");
        const auto original = *manager.inspect(*recover);
        (void)manager.command(*recover, {"crash"}, control::Clock::now() + 2s);
        check(eventually([&] { auto state = *manager.inspect("retry"); return state.state == ModuleState::Running && state.mid != original.mid; }), "running crash restarts into new session");
        check(manager.inspect("retry")->restarts == 1 && manager.inspect("retry")->name == original.name, "restart retains logical name");
        check(!manager.inspect(*recover) && !manager.command(*recover, {}, control::Clock::now() + 2s) && !manager.stop_module(*recover, control::Clock::now() + 1s), "stale MID cannot inspect, command or stop replacement");
        check(bool(manager.command("retry", {}, control::Clock::now() + 2s)), "new session command");
        check(bool(manager.stop_module("retry", control::Clock::now() + 3s)), "explicit stop after restart");
        std::this_thread::sleep_for(400ms);
        check(manager.inspect("retry")->state == ModuleState::Stopped && manager.inspect("retry")->restarts == 1, "explicit stop never restarts");
        check(bool(manager.unregister_module_type("helper") == 0), "unregister retry module type");
        {
            ModuleManager reserved;
            auto slow_retry = policy; slow_retry.restart_delay = 1200ms;
            check(bool(reserved.register_module_type({"helper", helper, "reservedInstance", slow_retry})), "restart reservation module type");
            auto first = reserved.start_module("helper");
            check(first && reserved.wait_started(*first) && *first != *recover, "MIDs unique across managers");
            (void)reserved.command(*first, {"crash"}, control::Clock::now() + 2s);
            check(eventually([&] { return reserved.inspect("reservedInstance")->state == ModuleState::RestartWaiting; }), "restart waiting observed");
            auto thief = reserved.start_module("helper");
            auto rejected = reserved.wait_started(*thief);
            check(!rejected && rejected.error().message.find("already exists") != std::string::npos, "name reserved between restarts");
            check(bool(reserved.wait_running("reservedInstance", control::Clock::now() + 5s)), "reserved Module restarted");
            check(reserved.inspect("reservedInstance")->mid != *first && !reserved.inspect(*first), "restart obtains fresh MID");
        }
        policy.max_restarts = 0;
        check(bool(manager.register_module_type({"helper", helper.parent_path() / "does-not-exist.exe", "missing", policy})), "register missing executable");
        auto missing = manager.start_module("helper"); check(bool(missing), "queue missing executable");
        check(eventually([&] { return manager.inspect(*missing)->state == ModuleState::Failed; }), "spawn failure bounded");
        check(manager.inspect(*missing)->os_error.has_value(), "native launch error retained");
        check(bool(manager.unregister_module_type("helper") == 0), "unregister missing module type");
        policy.max_restarts = 0;
        check(bool(manager.register_module_type({"silent", silent_helper, "silentNode", policy})), "register silent peer");
        auto silent = manager.start_module("silent"); check(bool(silent), "start silent peer");
        check(bool(manager.wait_running(*silent, control::Clock::now() + 3s)), "silent peer handshake");
        check(eventually([&] { return manager.inspect(*silent)->state == ModuleState::Failed; }), "central kills silent module");
        check(manager.inspect(*silent)->last_error == "heartbeat timeout" && manager.inspect(*silent)->last_exit.has_value(), "heartbeat cause and reaped exit retained");

        check(bool(manager.register_module_type({"helper", helper, "graceful", policy})), "register graceful helper");
        const auto marker = std::filesystem::temp_directory_path() / ("ov-stop-" + std::to_string(std::random_device{}()) + ".txt");
        const auto marker_utf8 = marker.u8string();
        auto graceful = manager.start_module("helper", {"marker", std::string(reinterpret_cast<const char*>(marker_utf8.data()), marker_utf8.size())});
        check(graceful && manager.wait_running(*graceful, control::Clock::now() + 3s), "graceful ready");
        check(bool(manager.stop_module(*graceful, control::Clock::now() + 3s)), "graceful stop");
        check(manager.inspect(*graceful)->last_exit && manager.inspect(*graceful)->last_exit->value == 0, "normal exit code");
        std::ifstream marker_file(marker); std::string stopped_name; marker_file >> stopped_name; marker_file.close();
        std::filesystem::remove(marker); check(stopped_name == manager.inspect(*graceful)->name, "on_stop executed");
        {
            ModuleManager scoped;
            check(bool(scoped.register_module_type({"helper", helper, "scopeNode", policy})), "scoped registration");
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
            spawn.arguments = {"--vinc-module-v2", endpoint_text, "orphan", "orphan-test", "1234", "40", "400", "2000", "200", "helper"};
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
                for (const auto& f : *frames) {
                    if (f.kind == control::Kind::Request && f.domain == 1 && f.operation == 3) {
                        control::Frame approved{control::Kind::Response, 1, 3, f.request};
                        control::put_u32(approved.payload, 0); control::put_string(approved.payload, "orphan");
                        const auto encoded = control::encode(approved); std::size_t sent = 0;
                        while (sent < encoded.size()) {
                            auto n = socket->send_until(std::span<const std::byte>(encoded).subspan(sent), control::Clock::now() + 1s);
                            check(n && *n, "approve orphan name"); sent += *n;
                        }
                    }
                    if (f.kind == control::Kind::Event && f.domain == 1 && f.operation == 1) ready = true;
                }
            }
            if (disconnect) check(bool(socket->close()), "close central socket");
            auto exit = child->wait_until(control::Clock::now() + 3s);
            check(exit && exit->value == 72, disconnect ? "module exits when central disconnects" : "module exits when central heartbeat stops");
        }
        std::cout << "module process lifecycle, naming, commands, isolation, timeout and restart tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
