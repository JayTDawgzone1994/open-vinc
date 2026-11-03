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

void startup_and_save(const std::filesystem::path& plugin) {
    Directory temporary;
    const auto name = unique_name();
    const auto file = temporary.path / name / "params.toml";
    auto manager = create(name, plugin, temporary.path);
    const auto absent = Access::param_command(*manager, {"set", "startup.gain", "8"});
    check(!absent && absent.error().is(module_system::ErrorCode::ParamFailed) &&
          absent.error().causes().size() == 1 &&
          absent.error().causes().front().domain == ErrorDomain::Param &&
          absent.error().causes().front().code == static_cast<std::int32_t>(param::ErrorCode::NotFound),
          "first startup cannot set an unregistered parameter");

    const auto launch = [&](std::vector<std::string> args) {
        const auto mid = take(manager->start_module("paramTest", std::move(args)));
        take(manager->wait_running(mid, deadline()));
        return mid;
    };
    auto owner = launch({"startup", "--gain", "8"});
    check(command(*manager, owner, {"effective"}) == 8, "startup override applied before Ready");
    auto image = take(Access::param_snapshot(*manager));
    check(std::get<std::int32_t>(image.entries.at("startup.gain").default_value) == 5,
          "args do not replace registered default");
    check(saved_value(file, "startup.gain") == 5, "startup override does not save");
    stop(*manager, owner);
    owner = launch({"startup"});
    check(command(*manager, owner, {"effective"}) == 8, "restart reads current without args");
    shutdown(*manager);
    manager.reset();

    manager = create(name, plugin, temporary.path);
    owner = launch({"startup"});
    check(command(*manager, owner, {"effective"}) == 5, "central restart drops unsaved startup override");
    stop(*manager, owner);

    // A stopped owner cannot validate before save. Command completion is the
    // ordering barrier; no sleep guesses when a callback will run.
    take(Access::param_command(*manager, {"set", "startup.gain", "-4"}));
    take(Access::param_command(*manager, {"save"}));
    check(saved_value(file, "startup.gain") == -4, "save does not wait for owner approval");
    owner = launch({"startup"});
    check(command(*manager, owner, {"effective"}) == 5, "owner corrects stored business-invalid value");
    check(saved_value(file, "startup.gain") == -4, "correction does not implicitly save");
    take(Access::param_command(*manager, {"save"}));
    check(saved_value(file, "startup.gain") == 5, "later explicit save captures correction");
    shutdown(*manager);
}

void script_and_socket(ModuleManager& manager, shell::CommandRouter& router,
                       const std::filesystem::path& root, std::string_view target) {
    const auto annotation = "[restart: " + std::string(target) + "]";
    const auto reminder = "Restart hint: " + std::string(target);
    const auto script = root / "params.tcl";
    {
        std::ofstream stream(script);
        stream << "param set camera.fixed 12\nparam get camera.fixed\nparam list\n";
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
    const auto script_output = runtime.take_output();
    check(script_output.find("= 12") != std::string::npos, "Tcl reads current parameter");
    check(script_output.find(reminder) != std::string::npos &&
          script_output.find(annotation) != std::string::npos, "Tcl retains restart metadata in replies");

    shell::ScriptService scripts(manager);
    take(scripts.initialize());
    shell::SocketShellOptions options;
    options.port = 0;
    auto server = take(shell::SocketShell::create(manager, scripts, options));
    auto client = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    take(client.connect_until(server->endpoint(), deadline()));
    std::string pending;
    const auto receive_prompt = [&](std::chrono::steady_clock::time_point limit) {
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
    receive_prompt(deadline());
    const auto receive_reply = [&] {
        const auto limit = deadline();
        while (std::chrono::steady_clock::now() < limit) {
            auto reply = receive_prompt(limit);
            // Asynchronous Param diagnostics also redraw the prompt. They are
            // not the reply to our command, notably after a type migration.
            if (!reply.starts_with("[Param] ")) {
                return reply;
            }
        }
        throw std::runtime_error("Socket Shell command reply timed out");
    };
    const auto send = [&](std::string_view text) {
        auto bytes = std::as_bytes(std::span(text));
        while (!bytes.empty()) {
            const auto count = take(client.send_until(bytes, deadline()));
            check(count != 0, "Socket Shell send progresses");
            bytes = bytes.subspan(count);
        }
    };
    send("param set camera.fixed 11\r\n");
    const auto set_reply = receive_reply();
    check(set_reply.find("not saved") != std::string::npos && set_reply.find(reminder) != std::string::npos,
          ("Socket Shell expected " + reminder + "; received: " + set_reply).c_str());
    if (target == "App") {
        check(set_reply.find("param save") != std::string::npos, "App restart reminder explains persistence");
    }
    send("param get camera.fixed\r\n");
    const auto get_reply = receive_reply();
    check(get_reply.find("= 11") != std::string::npos && get_reply.find(annotation) != std::string::npos,
          "Socket Shell reads parameter with annotation");
    send("param list\r\n");
    check(receive_reply().find(annotation) != std::string::npos, "Socket Shell list includes restart annotation");
    send("param set camera.fixed invalid\r\n");
    check(receive_reply().find("Restart hint:") == std::string::npos,
          "Socket Shell failed write has no restart success reminder");
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
    command(*manager, owner, {"write", "fixed", "6"});
    check(command(*manager, owner, {"get", "fixed"}) == 6, "Limited owner can write");
    check(take(Access::param_snapshot(*manager)).entries.at("camera.fixed").restart_hint == param::RestartHint::Module,
          "ordinary set preserves restart metadata");
    command(*manager, owner, {"write", "fixed", "5"});
    check(command(*manager, watcher, {"denied", "camera.fixed"}) == 1, "other Module denied");
    command(*manager, watcher, {"write", "camera.open", "7"});
    check(command(*manager, owner, {"get", "open"}) == 7, "Public permits every Module");
    check(saved_value(file, "camera.open") == 1, "runtime write never silently saves");
    const auto callbacks = command(*manager, owner, {"coalesce"});
    check(command(*manager, owner, {"get", "open"}) == 30, "latest value after coalescing");
    check(command(*manager, owner, {"callbacks"}) == callbacks + 1,
          "one callback for three commits before pump");
    command(*manager, owner, {"add"});
    check(command(*manager, watcher, {"current", "camera.extra"}) == 8,
          "unwatched foreign Limited value can be read");
    check(command(*manager, owner, {"parallel_reads"}) == 0, "concurrent Host snapshots");
    check(saved_value(file, "camera.open") == 1 && saved_value(file, "camera.extra") == 8,
          "auto addition preserves unsaved runtime edits");
    take(router.execute({"param", "save"}));
    check(saved_value(file, "camera.open") == 30, "explicit save captures live values");
    take(router.execute({"param", "set", "camera.fixed", "11"}));
    check(command(*manager, owner, {"get", "fixed"}) == 11, "central can change protected parameter");
    check(saved_value(file, "camera.fixed") == 5, "central changes need save too");
    const auto unchanged = take(router.execute({"param", "set", "camera.fixed", "11"}));
    check(unchanged.code == 0 && unchanged.message.find("Restart hint: Module 'camera'") != std::string::npos,
          "even a repeated value reports a declaration, not pending-restart state");
    const auto plain = take(router.execute({"param", "set", "camera.open", "30"}));
    check(plain.message == "Parameter committed (not saved)", "None retains compact success response");
    check(take(router.execute({"param", "get", "camera.open"})).message.find("restart:") == std::string::npos,
          "None does not claim immediate application or display a restart requirement");
    const auto before_failure = take(Access::param_snapshot(*manager));
    check(!router.execute({"param", "set", "camera.fixed", "invalid"}), "invalid set remains a failure");
    check(!router.execute({"param", "set", "missing.value", "1"}), "missing parameter remains a failure");
    check(take(Access::param_snapshot(*manager)).revision == before_failure.revision,
          "failed writes publish neither value nor metadata");
    script_and_socket(*manager, router, temporary.path, "Module 'camera'");
    check(manager->inspect(owner)->state == ModuleState::Running, "restart hint never stops the owner");
    check(saved_value(file, "camera.fixed") == 5, "restart reminders never imply save");
    const auto future = start(*manager, "future");
    check(command(*manager, watcher, {"get", "future.value"}) == 42, "missing watch repaired by version");
    stop(*manager, future);
    check(command(*manager, watcher, {"get", "future.value"}) == 42, "Module stop retains last parameter");
    stop(*manager, owner);
    check(take(Access::param_snapshot(*manager)).entries.at("camera.fixed").restart_hint == param::RestartHint::Module,
          "stopped owner retains restart metadata");
    owner = start(*manager, "camera", "2");
    command(*manager, owner, {"write", "fixed", "13"});
    check(command(*manager, owner, {"get", "fixed"}) == 13, "replacement MID owns Limited parameter");
    command(*manager, owner, {"write", "fixed", "11"});
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
    check(snapshot.entries.at("camera.fixed").restart_hint == param::RestartHint::None,
          "TOML does not restore runtime registration hints");
    check(snapshot.entries.contains("future.value") && snapshot.entries.contains("camera.extra"),
          "inactive/unregistered persisted items remain");
    owner = start(*manager, "camera", "double");
    snapshot = take(Access::param_snapshot(*manager));
    check(snapshot.entries.at("camera.fixed").restart_hint == param::RestartHint::App,
          "new registration replaces restart metadata");
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
    shell::CommandRouter restarted_router(*manager);
    take(restarted_router.initialize());
    script_and_socket(*manager, restarted_router, temporary.path, "App");
    check(manager->inspect(owner)->state == ModuleState::Running, "App hint does not shut down central or owner");
    check(saved_value(file, "camera.fixed") == 5, "App hint does not save automatically");
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
        startup_and_save(std::filesystem::absolute(argv[1]));
        run(std::filesystem::absolute(argv[1]));
        std::cout
            << "PASS: Param Host/Plugin, pump, permissions, persistence, restart and callback isolation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
