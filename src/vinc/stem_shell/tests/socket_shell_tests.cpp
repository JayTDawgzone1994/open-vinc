// Real OSAL TCP clients exercise session ownership and framing. No mock sockets.
#include "input_pipe.hpp"
#include <stem_shell/stem_shell.hpp>
#include <vinc/osal/socket.hpp>
#include <vinc/osal/random.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
template<class T> T take(osal::Result<T> value) { if (!value) throw std::runtime_error(value.error().native.message()); return std::move(*value); }
void take(osal::Result<void> value) { if (!value) throw std::runtime_error(value.error().native.message()); }
template<class F> bool eventually(F f) {
    const auto end = Clock::now() + 5s;
    while (Clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(10ms); } return false;
}
std::string text(const std::filesystem::path& p) { auto s = p.generic_u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
struct Files {
    std::filesystem::path path = std::filesystem::current_path() / ("socket-shell-" + std::to_string(Clock::now().time_since_epoch().count()));
    Files() { std::filesystem::create_directory(path); }
    ~Files() { std::error_code e; std::filesystem::remove_all(path, e); }
    std::filesystem::path write(std::string name, std::string source) { auto p = path / name; std::ofstream(p) << source; return p; }
};
struct Client {
    osal::Socket socket;
    std::string pending, code;
    explicit Client(osal::IpEndpoint address, int receive_buffer = 0) {
        socket = take(osal::Socket::open(osal::address_family(address), osal::SocketType::Stream));
        if (receive_buffer) take(socket.set_option<osal::socket_options::ReceiveBuffer>(receive_buffer));
        take(socket.connect_until(address, Clock::now() + 2s));
        auto greeting = until("VINC > ");
        auto at = greeting.find("Resume code: "); check(at != std::string::npos, "greeting includes recovery code");
        code = greeting.substr(at + 13, 64); check(code.size() == 64, "256-bit recovery token");
    }
    void send(std::string_view value) {
        auto bytes = std::as_bytes(std::span(value));
        while (!bytes.empty()) { auto n = take(socket.send_until(bytes, Clock::now() + 3s)); check(n != 0, "send progress"); bytes = bytes.subspan(n); }
    }
    std::string until(std::string_view needle, std::chrono::milliseconds timeout = 3s) {
        auto deadline = Clock::now() + timeout;
        while (pending.find(needle) == std::string::npos) {
            std::array<std::byte, 8192> bytes{};
            auto got = socket.receive_until(bytes, deadline);
            if (!got || got->eof) throw std::runtime_error("TCP receive waiting for: " + std::string(needle) + " buffered: " + pending);
            pending.append(reinterpret_cast<char*>(bytes.data()), got->bytes);
            check(pending.size() < 2 * 1024 * 1024, "bounded test buffer");
        }
        auto n = pending.find(needle) + needle.size(); auto result = pending.substr(0, n); pending.erase(0, n); return result;
    }
    std::string command(std::string line) { send(line + "\r\n"); return until("VINC > "); }
    void close() { take(socket.close()); }
};
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        shell::initialize_tcl(argv[0]); Files files;
        std::array<std::byte, 600> random1{}, random2{};
        take(osal::random_bytes(random1)); take(osal::random_bytes(random2));
        check(random1 != random2, "OS random source fills multiple chunks"); take(osal::random_bytes({}));
        {
            ModuleManager manager; shell::ScriptService scripts(manager);
            shell::SocketShellOptions options; options.port = 0; options.disconnect_timeout = 1200ms; options.send_timeout = 300ms;
            shell::SocketShell server(manager, scripts, options);
            check(server.endpoint().port != 0 && std::get<osal::IPv4Address>(server.endpoint().address) == osal::IPv4Address::loopback(), "default loopback and actual port");
            bool failed = false;
            try { auto duplicate = options; duplicate.port = server.endpoint().port; shell::SocketShell conflict(manager, scripts, duplicate); }
            catch (const std::exception&) { failed = true; }
            check(failed, "bind conflict fails startup");
            Client a(server.endpoint()), b(server.endpoint());
            check(a.code != b.code, "sessions have distinct recovery tokens");
            check(a.command("output off").find("off") != std::string::npos, "output off");
            a.send("hel"); std::this_thread::sleep_for(20ms); a.send("p\r");
            auto help = a.until("VINC > "); check(help.find("[Commands]") != std::string::npos, "fragmented command and CR line ending");
            a.send("\n"); // The LF completing CRLF must not execute a second empty command.
            check(a.command("no_such_command").find("Unknown command") != std::string::npos, "CRLF consumed once");
            check(a.command(std::string(4097, 'x')).find("oversized") != std::string::npos, "oversized line rejected entirely");
            check(a.command(std::string("help\0exit", 9)).find("Rejected") != std::string::npos, "NUL rejected");
            check(a.command("\xc0\xaf").find("Rejected") != std::string::npos, "invalid UTF-8 rejected");
            check(!server.shutdown_requested(), "invalid lines cannot execute hidden exit");
            check(a.command("help").find("[Commands]") != std::string::npos, "parser recovers after rejected lines");

            auto forever = files.write("forever.tcl", "puts owner-only-output\nwhile {1} {after 10}\n");
            a.send("srun " + shell::quote_argument(text(forever)) + "\n");
            a.until("Script running."); a.until("owner-only-output"); a.pending.clear();
            check(b.command("sstatus").find("Script running") != std::string::npos, "another session can inspect the job");
            check(b.command("srun " + shell::quote_argument(text(forever))).find("Busy") != std::string::npos, "global script exclusion");
            check(b.pending.find("owner-only-output") == std::string::npos, "script output stays with initiating session");
            check(!scripts.start(1, forever, {}).success, "local script shares the same exclusion");
            const auto old_code = a.code; a.close();
            std::this_thread::sleep_for(100ms); check(scripts.active(), "script continues during reconnect grace");
            Client resumed(server.endpoint());
            auto restored = resumed.command("resume " + old_code);
            check(restored.find("Session resumed") != std::string::npos, "resume restores disconnected owner");
            auto at = restored.find("Resume code: "); resumed.code = restored.substr(at + 13, 64);
            check(resumed.code != old_code, "resume rotates recovery code");
            check(b.command("resume " + old_code).find("rejected") != std::string::npos, "old code cannot be reused");
            std::this_thread::sleep_for(400ms); // No queued output: send timeout must not disconnect an idle client.
            check(resumed.command("help").find("[Commands]") != std::string::npos, "idle connections survive send timeout");
            const auto expiring = resumed.code; resumed.close();
            check(eventually([&] { return !scripts.active(); }), "disconnect expiry cancels only the owner script");
            check(b.command("resume " + expiring).find("rejected") != std::string::npos, "expired sessions cannot resume");
            check(scripts.start(1, forever, {}).success, "start local script after remote expiry");
            { Client unrelated(server.endpoint()); unrelated.close(); }
            std::this_thread::sleep_for(1400ms);
            check(scripts.active(1), "unrelated remote disconnect cannot cancel local job");
            check(b.command("scancel").find("requested") != std::string::npos, "remote cancellation reaches global job");
            check(eventually([&] { return !scripts.active(); }), "cooperative global cancellation completes");
            auto local = scripts.take(1); check(local.result && local.result->canceled, "local owner receives cancellation result");
            // A client that stops reading must not hold the network worker or its script forever.
            Client stalled(server.endpoint(), 1024);
            auto flood = files.write("flood.tcl", "while {1} {puts [string repeat x 65536]}\n");
            stalled.send("srun " + shell::quote_argument(text(flood)) + "\n"); stalled.until("Script running.");
            check(b.command("help").find("[Commands]") != std::string::npos, "stalled peer cannot block another connection");
            check(eventually([&] { return !scripts.active(); }), "send stall disconnects and expiry cancels flood script");
            server.begin_shutdown(); server.finish_shutdown();
        }
        {
            ModuleManager manager; std::ostringstream local_output; StemShell local(manager, local_output);
            shell::SocketShellOptions options; options.port = 0;
            auto address = local.start_socket_shell(options);
            std::vector<std::unique_ptr<Client>> clients;
            for (int i = 0; i < 8; ++i) {
                auto client = std::make_unique<Client>(address);
                check(client->command("output on").find("Module output on") != std::string::npos, "eight remote output subscriptions plus local subscription");
                clients.push_back(std::move(client));
            }
            auto ninth = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
            take(ninth.connect_until(address, Clock::now() + 2s));
            std::array<std::byte, 64> bytes{};
            auto received = ninth.receive_until(bytes, Clock::now() + 2s);
            check((received && received->eof) || (!received && received.error().code != osal::ErrorCode::Timeout), "ninth connection is closed at capacity");
            local.wait_shutdown();
        }
        {
            ModuleManager manager; shell::ScriptService scripts(manager);
            shell::SocketShellOptions options; options.ip = "::1"; options.port = 0;
            shell::SocketShell server(manager, scripts, options); Client client(server.endpoint());
            check(client.command("help").find("[Commands]") != std::string::npos, "configured IPv6 listener");
        }
        {
            // Full local event loop: remote exit must wait for the actual child stop hook.
            ModuleManager manager; InputPipe input; std::ostringstream output; StemShell shell(manager, output);
            check(bool(manager.register_module_type({"shellHelper", std::filesystem::absolute(argv[1]), "socketNode", {}})), "register module");
            check(!manager.register_module_type({"resume", std::filesystem::absolute(argv[1]), "reserved", {}}), "remote names reserved");
            shell::SocketShellOptions options; options.port = 0;
            const auto endpoint = shell.start_socket_shell(options);
            auto loop = std::async(std::launch::async, [&] { shell.run(); });
            struct Guard { InputPipe& input; std::future<void>& loop; ~Guard() { if (loop.valid()) { input.send("\x03"); loop.wait(); } } } guard{input, loop};
            Client a(endpoint), b(endpoint);
            b.command("output off");
            a.send("shellHelper output " + shell::quote_argument(text(files.path)) + "\n");
            a.until("Started socketNode"); a.until("VINC > ");
            check(eventually([&] { return std::filesystem::exists(files.path / "socketNode.ready"); }), "remote starts real process");
            a.send("socketNode output\n");
            a.until("command-output\\x1B[2J"); a.pending.clear();
            auto result = b.command("help"); check(result.find("command-output") == std::string::npos, "muted module output does not hide command reply");
            a.send("socketNode slow\nsocketNode slow\n");
            a.until("Busy:");
            check(b.command("help").find("[Commands]") != std::string::npos, "slow command does not block other clients");
            b.send("exit\n");
            b.until("All module processes exited; framework shutdown complete.");
            a.until("stop-tail-without-newline");
            a.until("All module processes exited; framework shutdown complete.");
            check(loop.wait_for(5s) == std::future_status::ready, "remote exit completes framework loop"); loop.get();
            check(std::filesystem::exists(files.path / "socketNode.stopped"), "module stop hook completed before framework exit");
            check(output.str().find("stop-tail-without-newline") != std::string::npos, "local output drained during remote shutdown");
        }
        std::cout << "Socket Shell tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
