#include "support.hpp"
#include <vinc/module/detail/manager_access.hpp>
#include <vinc/log/detail/manager.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/shell/detail/socket_shell.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <array>
#include <fstream>
#include <iostream>
using namespace vinc;
using namespace log_test;

void socket_commands(ModuleManager& manager) {
    shell::CommandRouter router(manager);
    take(router.initialize());
    check(!take(router.execute({"log", "status"})).message.empty(), "Shell router log status");
    shell::ScriptService scripts(manager);
    take(scripts.initialize());
    shell::SocketShellOptions options;
    options.port = 0;
    auto server = take(shell::SocketShell::create(manager, scripts, options));
    auto client = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    take(client.connect_until(server->endpoint(), deadline()));
    std::string pending;
    auto receive = [&] {
        while (pending.find("VINC > ") == std::string::npos) {
            std::array<std::byte, 1024> bytes;
            auto received = take(client.receive_until(bytes, deadline()));
            check(!received.eof, "Socket Shell connected");
            for (std::size_t i = 0; i < received.bytes; ++i) {
                pending += static_cast<char>(bytes[i]);
            }
        }
        const auto count = pending.find("VINC > ") + std::string_view("VINC > ").size();
        auto text = pending.substr(0, count);
        pending.erase(0, count);
        return text;
    };
    receive();
    auto send = [&](std::string_view text) {
        auto bytes = std::as_bytes(std::span(text));
        while (!bytes.empty()) {
            const auto written = take(client.send_until(bytes, deadline()));
            check(written != 0, "Socket send progresses");
            bytes = bytes.subspan(written);
        }
    };
    send("log status\r\n");
    check(receive().find("file=enabled") != std::string::npos, "remote status");
    send("log show debug\r\n");
    check(receive().find("display level updated") != std::string::npos, "remote display level");
    send("log show module camera\r\n");
    check(receive().find("module filter updated") != std::string::npos, "remote source filter");
    send("log show on\r\n");
    check(receive().find("Log display on") != std::string::npos, "remote display enabled");
    take(client.close());
    server->begin_shutdown();
    server->finish_shutdown();
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "plugin required");
        Directory directory;
        const std::string name = "log-test";
        auto manager = take(detail::ManagerAccess::create(name));
        const auto blocked = directory.path / name / "logs";
        std::filesystem::create_directories(blocked.parent_path());
        {
            std::ofstream stream(blocked);
            stream << "blocker";
        }
        check(!detail::ManagerAccess::configure_log(*manager, directory.path),
              "file failure does not kill central");
        check(take(detail::ManagerAccess::log_command(*manager, {"status"})).find("file=disabled") !=
                  std::string::npos,
              "file failure visible");
        std::filesystem::remove(blocked);
        take(detail::ManagerAccess::log_command(*manager, {"reopen"}));
        const auto reopen_deadline = deadline();
        bool reopened = false;
        while (std::chrono::steady_clock::now() < reopen_deadline) {
            reopened = take(detail::ManagerAccess::log_command(*manager, {"status"})).find("file=enabled") !=
                       std::string::npos;
            if (reopened) {
                break;
            }
            std::this_thread::sleep_for(poll);
        }
        check(reopened, "explicit reopen recovers file output");
        ModuleType type{"logTest", take(osal::path_from_utf8(argv[1]))};
        type.policy.heartbeat_interval = 50ms;
        type.policy.heartbeat_timeout = 1000ms;
        take(manager->register_module_type(type));
        const auto mid = take(manager->start_module("logTest"));
        take(manager->wait_running(mid, deadline()));
        take(detail::ManagerAccess::log_command(*manager, {"level", "camera", "debug"}));
        const auto end = deadline();
        bool enabled = false;
        while (std::chrono::steady_clock::now() < end) {
            enabled = take(manager->command(mid, {"debug"}, deadline())) == 1;
            if (enabled) {
                break;
            }
            std::this_thread::sleep_for(poll);
        }
        check(enabled, "source level received independently of pump");
        log::View view;
        take(log::detail::view_command(view, {"log", "show", "on"}));
        std::string displayed;
        const auto display_deadline = deadline();
        while (displayed.find("structured-start 42") == std::string::npos &&
               std::chrono::steady_clock::now() < display_deadline) {
            displayed += detail::ManagerAccess::log_take(*manager, view);
            std::this_thread::sleep_for(poll);
        }
        check(displayed.find("structured-start 42") != std::string::npos, "structured display subscription");
        check(displayed.find("raw-start") == std::string::npos, "printf kept separate from log display");
        socket_commands(*manager);
        take(manager->command(mid, {"flood"}, deadline()));
        const auto catchup_deadline = deadline();
        bool caught_up = false;
        while (std::chrono::steady_clock::now() < catchup_deadline) {
            take(manager->command(mid, {"barrier"}, deadline()));
            caught_up = detail::ManagerAccess::log_take(*manager, view).find("after-flood-marker") !=
                        std::string::npos;
            if (caught_up) {
                break;
            }
            std::this_thread::sleep_for(poll);
        }
        check(caught_up, "log collection recovers after flood");
        check(take(manager->command(mid, {"ping"}, deadline())) == 7, "control remains usable after flood");
        take(manager->stop_module(mid, deadline()));
        manager.reset();
        std::string saved;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(directory.path / name / "logs")) {
            if (entry.is_regular_file()) {
                std::ifstream input(entry.path(), std::ios::binary);
                saved.append(std::istreambuf_iterator<char>(input), {});
            }
        }
        check(saved.find("structured-start 42") != std::string::npos, "structured record saved");
        check(saved.find("raw-start") != std::string::npos, "printf saved");
        check(saved.find("raw-stop") != std::string::npos, "shutdown printf drained");
        check(saved.find("structured-stop") != std::string::npos, "shutdown structured log fenced");
        check(saved.find("debug-enabled") != std::string::npos, "debug capture enabled");
        check(saved.find("module=camera mid=") != std::string::npos,
              "authenticated source assigned centrally");
        std::cout << "Log process tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
