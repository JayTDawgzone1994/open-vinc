#include "support.hpp"
#include <vinc/module/detail/manager_access.hpp>
#include "../../../../tests/support/manager.hpp"
#include <vinc/param/detail/persistence.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <vinc/shell/detail/socket_shell.hpp>
#include <vinc/osal/path.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace vinc;
using namespace param_test;
using Access = vinc::detail::ManagerAccess;

MID start(ModuleManager& manager, std::string name, std::string defaults = "1") {
    const auto mid = take(manager.start_module("paramTest", {std::move(name), std::move(defaults)}));
    take(manager.wait_running(mid, deadline()));
    return mid;
}

std::int32_t command(ModuleManager& manager, MID mid, std::vector<std::string> args) {
    return take(manager.command(mid, std::move(args), deadline()));
}

std::unique_ptr<ModuleManager> create(std::string name, const std::filesystem::path& plugin,
                                      const std::filesystem::path& root) {
    auto manager = take(Access::create(std::move(name), {}, vinc::test::host_path()));
    take(Access::configure_params(*manager, root));
    ModuleType type{"paramTest", plugin};
    take(manager->register_module_type(std::move(type)));
    return manager;
}

std::int32_t saved_value(const std::filesystem::path& file, const std::string& name) {
    param::detail::Persistence reader;
    return std::get<std::int32_t>(take(reader.load(file)).at(name).value);
}

void stop(ModuleManager& manager, MID mid) {
    take(manager.stop_module(mid, deadline()));
}

void shutdown(ModuleManager& manager) {
    manager.begin_shutdown();
    take(manager.wait_shutdown(deadline()));
}

void script_and_socket(ModuleManager& manager, shell::CommandRouter& router,
                       const std::filesystem::path& root) {
    const auto script = root / "params.tcl";
    {
        std::ofstream stream(script);
        stream << "param set camera.fixed 12\nputs [param get camera.fixed]\n";
    }
    shell::TclRuntime runtime;
    take(runtime.start(script, {}, [&](const auto& args, std::stop_token stop) {
        return router.execute(args, stop);
    }));
    const auto end = deadline();
    while (!runtime.ready() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(poll_interval);
    }
    check(runtime.ready(), "Param Tcl job completed");
    const auto report = runtime.finish();
    take(report.outcome);
    check(runtime.take_output().find("= 12") != std::string::npos, "Tcl reads current parameter");

    shell::ScriptService scripts(manager);
    take(scripts.initialize());
    shell::SocketShellOptions options;
    options.port = 0;
    auto server = take(shell::SocketShell::create(manager, scripts, options));
    auto client = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    take(client.connect_until(server->endpoint(), deadline()));
    std::string pending;
    const auto receive_prompt = [&] {
        const auto limit = deadline();
        while (pending.find("VINC > ") == std::string::npos) {
            std::array<std::byte, 1024> buffer{};
            const auto received = take(client.receive_until(buffer, limit));
            check(!received.eof, "Socket Shell remains connected");
            for (std::size_t index = 0; index < received.bytes; ++index) {
                pending += static_cast<char>(std::to_integer<unsigned char>(buffer[index]));
            }
        }
        const auto consumed = pending.find("VINC > ") + std::string_view("VINC > ").size();
        auto reply = pending.substr(0, consumed);
        pending.erase(0, consumed);
        return reply;
    };
    receive_prompt();
    const auto send = [&](std::string_view text) {
        auto bytes = std::as_bytes(std::span(text));
        while (!bytes.empty()) {
            const auto count = take(client.send_until(bytes, deadline()));
            check(count != 0, "Socket Shell send progresses");
            bytes = bytes.subspan(count);
        }
    };
    send("param set camera.fixed 11\r\n");
    check(receive_prompt().find("not saved") != std::string::npos, "Socket Shell writes parameter");
    send("param get camera.fixed\r\n");
    check(receive_prompt().find("= 11") != std::string::npos, "Socket Shell reads parameter");
    take(client.close());
    server->begin_shutdown();
    server->finish_shutdown();
}

void run(const std::filesystem::path& plugin) {
    Directory temporary;
    const auto name = unique_name();
    const auto file = temporary.path / name / "params.toml";
    auto manager = create(name, plugin, temporary.path);
    check(std::filesystem::exists(file), "normal startup persists empty file");
    shell::CommandRouter router(*manager);
    take(router.initialize());
    const auto watcher = start(*manager, "watcher");
    check(command(*manager, watcher, {"missing", "camera.open"}) == 1, "watch before publisher exists");
    auto owner = start(*manager, "camera");
    check(command(*manager, watcher, {"get", "camera.open"}) == 1, "late registration becomes visible");
    check(command(*manager, owner, {"denied", "fixed"}) == 1, "owner cannot modify framework-only parameter");
    check(command(*manager, watcher, {"denied", "camera.fixed"}) == 1, "other Module denied");
    command(*manager, watcher, {"write", "camera.open", "7"});
    check(command(*manager, owner, {"get", "open"}) == 7, "Open permits every Module");
    check(saved_value(file, "camera.open") == 1, "runtime write never silently saves");
    const auto callbacks = command(*manager, owner, {"coalesce"});
    check(command(*manager, owner, {"get", "open"}) == 30, "latest value after coalescing");
    check(command(*manager, owner, {"callbacks"}) == callbacks + 1,
          "one callback for three commits before pump");
    command(*manager, owner, {"add"});
    check(saved_value(file, "camera.open") == 1 && saved_value(file, "camera.extra") == 8,
          "auto addition preserves unsaved runtime edits");
    take(router.execute({"param", "save"}));
    check(saved_value(file, "camera.open") == 30, "explicit save captures live values");
    take(router.execute({"param", "set", "camera.fixed", "11"}));
    check(command(*manager, owner, {"get", "fixed"}) == 11, "central can change protected parameter");
    check(saved_value(file, "camera.fixed") == 5, "central changes need save too");
    script_and_socket(*manager, router, temporary.path);
    const auto future = start(*manager, "future");
    check(command(*manager, watcher, {"get", "future.value"}) == 42, "missing watch repaired by version");
    stop(*manager, future);
    check(command(*manager, watcher, {"get", "future.value"}) == 42, "Module stop retains last parameter");
    stop(*manager, owner);
    owner = start(*manager, "camera", "2");
    check(command(*manager, owner, {"get", "open"}) == 30, "new MID/default preserves customized current");
    check(command(*manager, owner, {"get", "fixed"}) == 11, "same central lifetime retains unsaved value");
    param::detail::Persistence inspect;
    const auto saved = take(inspect.load(file));
    check(std::get<std::int32_t>(saved.at("camera.open").default_value) == 2 &&
              std::get<std::int32_t>(saved.at("camera.open").value) == 30,
          "new default with saved override");
    command(*manager, owner, {"arm"});
    take(router.execute({"param", "set", "camera.open", "31"}));
    const auto end = deadline();
    while (std::chrono::steady_clock::now() < end) {
        const auto snapshot = manager->inspect(owner);
        if (snapshot && snapshot->state == ModuleState::Failed) {
            check(snapshot->last_failure.has_value(), "callback fault retained");
            break;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    check(manager->inspect(owner)->state == ModuleState::Failed, "bad callback terminates only its Module");
    check(command(*manager, watcher, {"get", "camera.open"}) == 31,
          "other Module and central survive callback fault");
    shutdown(*manager);
    manager.reset();

    // A fresh central reconstructs saved values before any Module starts.
    manager = create(name, plugin, temporary.path);
    auto snapshot = take(Access::param_snapshot(*manager));
    check(take(snapshot.entries.at("camera.open").get<std::int32_t>()) == 30,
          "restart uses saved, not live value");
    check(take(snapshot.entries.at("camera.fixed").get<std::int32_t>()) == 5,
          "unsaved edits disappear at restart");
    check(snapshot.entries.contains("future.value") && snapshot.entries.contains("camera.extra"),
          "inactive/unregistered persisted items remain");
    owner = start(*manager, "camera", "double");
    snapshot = take(Access::param_snapshot(*manager));
    check(take(snapshot.entries.at("camera.open").get<double>()) == 2.5,
          "changed type resets only that item");
    check(snapshot.entries.contains("future.value"), "type migration preserves unrelated entries");
    const auto status = take(Access::param_command(*manager, {"status"}));
    check(status.find("type changed") != std::string::npos, "type migration diagnostic");
    std::uint64_t diagnostic_cursor = 0;
    check(Access::param_diagnostic(*manager, diagnostic_cursor).find("type changed") != std::string::npos,
          "automatic Shell diagnostic delivery");
    check(Access::param_diagnostic(*manager, diagnostic_cursor).empty(),
          "diagnostic delivered once per cursor");
    shutdown(*manager);
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 2, "Plugin path required");
        const auto executable = take(vinc::osal::path_to_utf8(std::filesystem::absolute(argv[0])));
        take(vinc::shell::initialize_tcl(executable.c_str()));
        run(std::filesystem::absolute(argv[1]));
        std::cout
            << "PASS: Param Host/Plugin, pump, permissions, persistence, restart and callback isolation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
