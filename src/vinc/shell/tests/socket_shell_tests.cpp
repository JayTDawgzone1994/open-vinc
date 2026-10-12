#include "../../../../tests/support/manager.hpp"
// Real OSAL TCP clients exercise session ownership and framing. No mock sockets.
#include "input_pipe.hpp"
#include <vinc/shell/detail/shell.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/socket.hpp>
#include <vinc/osal/random.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
// Test timing budgets and observation delays; chrono suffixes specify the units.
constexpr auto capacity_probe_timeout = 2s;
constexpr auto remote_shutdown_timeout = 5s;
} // namespace

using Clock = std::chrono::steady_clock;

template <class T> T checked_value(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    return std::move(*result);
}

template <class T> void check(const vinc::Result<T>& result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + vinc::error_to_string(result.error()));
    }
}

void check(bool condition, const char* what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
}

// Bound asynchronous fixture polling so a failed scenario cannot hang CTest.
constexpr auto fixture_wait_timeout = 5s;
constexpr auto fixture_poll_interval = 10ms;

template <class T> T take(vinc::Result<T> value) {
    if (!value) {
        throw std::runtime_error(vinc::error_to_string(value.error()));
    }

    return std::move(*value);
}

void take(vinc::Result<void> value) {
    if (!value) {
        throw std::runtime_error(vinc::error_to_string(value.error()));
    }
}

template <class F> bool eventually(F predicate) {
    const auto end = Clock::now() + fixture_wait_timeout;
    while (Clock::now() < end) {
        if (predicate()) {
            return true;
        }

        std::this_thread::sleep_for(fixture_poll_interval);
    }

    return false;
}

std::string text(const std::filesystem::path& path) {
    auto utf8 = path.generic_u8string();
    // char may read UTF-8 char8_t object bytes; copy while the owning string remains alive.
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

struct Files {
    std::filesystem::path path = std::filesystem::current_path() /
                                 ("socket-shell-" + std::to_string(Clock::now().time_since_epoch().count()));

    Files() {
        std::filesystem::create_directory(path);
    }

    ~Files() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path write(std::string name, std::string source) {
        auto file = path / take(osal::path_from_utf8(name));
        std::ofstream(file) << source;
        return file;
    }
};

constexpr std::string_view resume_code_prefix = "Resume code: ";
constexpr std::size_t resume_code_hex_chars = 64; // 256-bit token in hexadecimal.
constexpr std::size_t receive_chunk_bytes = 8192;
constexpr std::size_t test_pending_byte_limit = 2 * 1024 * 1024;
constexpr auto client_connect_timeout = 2s;
constexpr auto client_io_timeout = 3s;
constexpr std::uint64_t local_script_owner = 1;

struct Client {
    osal::Socket socket;
    std::string pending;
    std::string code;

    explicit Client(osal::IpEndpoint address, int receive_buffer = 0) {
        socket = take(osal::Socket::open(osal::address_family(address), osal::SocketType::Stream));
        if (receive_buffer) {
            take(socket.set_option<osal::socket_options::ReceiveBuffer>(receive_buffer));
        }

        take(socket.connect_until(address, Clock::now() + client_connect_timeout));
        auto greeting = until("VINC > ");
        auto code_start = greeting.find("Resume code: ");
        check(code_start != std::string::npos, "greeting includes recovery code");
        code = greeting.substr(code_start + resume_code_prefix.size(), resume_code_hex_chars);
        check(code.size() == resume_code_hex_chars, "256-bit recovery token");
    }

    void send(std::string_view value) {
        auto bytes = std::as_bytes(std::span(value));
        while (!bytes.empty()) {
            auto byte_count = take(socket.send_until(bytes, Clock::now() + client_io_timeout));
            check(byte_count != 0, "send progress");
            bytes = bytes.subspan(byte_count);
        }
    }

    std::string until(std::string_view needle, std::chrono::milliseconds timeout = client_io_timeout) {
        auto deadline = Clock::now() + timeout;
        while (pending.find(needle) == std::string::npos) {
            std::array<std::byte, receive_chunk_bytes> bytes{};
            auto received = socket.receive_until(bytes, deadline);
            if (!received || received->eof) {
                throw std::runtime_error("TCP receive waiting for: " + std::string(needle) +
                                         " buffered: " + pending);
            }

            // char may inspect the received byte objects. Copy only the initialized
            // prefix while this stack buffer is alive; no pointer escapes.
            pending.append(reinterpret_cast<char*>(bytes.data()), received->bytes);
            check(pending.size() < test_pending_byte_limit, "bounded test buffer");
        }

        auto byte_count = pending.find(needle) + needle.size();
        auto result = pending.substr(0, byte_count);
        pending.erase(0, byte_count);
        return result;
    }

    std::string command(std::string line) {
        send(line + "\r\n");
        return until("VINC > ");
    }

    void close() {
        take(socket.close());
    }
};

void test_sessions_and_backpressure(Files& files) {
    constexpr auto fragment_gap = 20ms;
    constexpr auto reconnect_probe_delay = 100ms;
    constexpr auto idle_probe_delay = 400ms;           // Longer than send_timeout with no pending output.
    constexpr auto expired_owner_probe_delay = 1400ms; // Longer than disconnect_timeout.
    constexpr int stalled_receive_buffer_bytes = 1024;

    auto manager_owner = vinc::test::create_manager();

    if (!manager_owner) {

        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }

    auto& manager = **manager_owner;
    shell::ScriptService scripts(manager);
    check(scripts.initialize(), "initialize script service");
    shell::SocketShellOptions options;
    options.port = 0;
    options.disconnect_timeout = 1200ms;
    options.send_timeout = 300ms;
    auto server_owner = checked_value(shell::SocketShell::create(manager, scripts, options));
    auto& server = *server_owner;
    check(server.endpoint().port != 0 &&
              std::get<osal::IPv4Address>(server.endpoint().address) == osal::IPv4Address::loopback(),
          "default loopback and actual port");
    auto duplicate = options;
    duplicate.port = server.endpoint().port;
    auto conflict = shell::SocketShell::create(manager, scripts, duplicate);
    check(!conflict && conflict.error().is(shell::ErrorCode::SocketFailed) &&
              conflict.error().is_operation(shell::ErrorOperation::Bind) &&
              conflict.error().causes().size() == 1 &&
              conflict.error().causes()[0].domain == vinc::ErrorDomain::Osal,
          "bind conflict preserves OSAL cause");
    Client owner_client(server.endpoint());
    Client observer_client(server.endpoint());
    check(owner_client.code != observer_client.code, "sessions have distinct recovery tokens");
    check(owner_client.command("output off").find("off") != std::string::npos, "output off");
    owner_client.send("hel");
    std::this_thread::sleep_for(fragment_gap);
    owner_client.send("p\r");
    auto help = owner_client.until("VINC > ");
    check(help.find("[Commands]") != std::string::npos, "fragmented command and CR line ending");
    owner_client.send("\n"); // The LF completing CRLF must not execute a second empty command.
    check(owner_client.command("no_such_command").find("Unknown command") != std::string::npos,
          "CRLF consumed once");
    check(owner_client.command(std::string(4097, 'x')).find("oversized") != std::string::npos,
          "oversized line rejected entirely");
    check(owner_client.command(std::string("help\0exit", 9)).find("Rejected") != std::string::npos,
          "NUL rejected");
    check(owner_client.command("\xc0\xaf").find("Rejected") != std::string::npos, "invalid UTF-8 rejected");
    check(!server.shutdown_requested(), "invalid lines cannot execute hidden exit");
    check(owner_client.command(std::string("srun ") + "\xa4\xa4.tcl").find("Rejected") != std::string::npos,
          "remote path does not guess legacy encoding");
    check(owner_client.command("help").find("[Commands]") != std::string::npos,
          "parser recovers after rejected lines");

    auto forever = files.write("遠端 😀.tcl", "puts owner-only-output\nwhile {1} {after 10}\n");
    owner_client.send("srun " + shell::quote_argument(text(forever)) + "\n");
    owner_client.until("Script running.");
    owner_client.until("owner-only-output");
    owner_client.pending.clear();
    check(observer_client.command("sstatus").find("Script running") != std::string::npos,
          "another session can inspect the job");
    check(observer_client.command("srun " + shell::quote_argument(text(forever))).find("Busy") !=
              std::string::npos,
          "global script exclusion");
    check(observer_client.pending.find("owner-only-output") == std::string::npos,
          "script output stays with initiating session");
    check(!scripts.start(local_script_owner, forever, {}).has_value(),
          "local script shares the same exclusion");
    const auto old_code = owner_client.code;
    owner_client.close();
    std::this_thread::sleep_for(reconnect_probe_delay);
    check(scripts.active(), "script continues during reconnect grace");
    Client resumed(server.endpoint());
    // A single TCP read can contain the handoff, another command and a partial next line.
    resumed.send("resume " + old_code + "\r\nsstatus\nout");
    auto restored = resumed.until("VINC > ");
    check(restored.find("Session resumed") != std::string::npos, "resume restores disconnected owner");
    check(resumed.until("VINC > ").find("Script running") != std::string::npos,
          "same-batch command survives resume");
    resumed.send("put off\r");
    check(resumed.until("VINC > ").find("Module output off") != std::string::npos,
          "partial next command survives resume");
    resumed.send("\nhelp\n");
    check(resumed.until("VINC > ").find("[Commands]") != std::string::npos,
          "split CRLF stays one delimiter after handoff");
    auto code_start = restored.find("Resume code: ");
    resumed.code = restored.substr(code_start + resume_code_prefix.size(), resume_code_hex_chars);
    check(resumed.code != old_code, "resume rotates recovery code");
    check(observer_client.command("resume " + old_code).find("rejected") != std::string::npos,
          "old code cannot be reused");
    std::this_thread::sleep_for(idle_probe_delay);
    check(resumed.command("help").find("[Commands]") != std::string::npos,
          "idle connections survive send timeout");
    const auto expiring = resumed.code;
    resumed.close();
    check(eventually([&] {
              return !scripts.active();
          }),
          "disconnect expiry cancels only the owner script");
    check(observer_client.command("resume " + expiring).find("rejected") != std::string::npos,
          "expired sessions cannot resume");
    check(scripts.start(local_script_owner, forever, {}).has_value(),
          "start local script after remote expiry");
    {
        Client unrelated(server.endpoint());
        unrelated.close();
    }

    std::this_thread::sleep_for(expired_owner_probe_delay);
    check(scripts.active(local_script_owner), "unrelated remote disconnect cannot cancel local job");
    check(observer_client.command("scancel").find("requested") != std::string::npos,
          "remote cancellation reaches global job");
    check(eventually([&] {
              return !scripts.active();
          }),
          "cooperative global cancellation completes");
    auto local = scripts.take(local_script_owner);
    check(local.result && !local.result->outcome &&
              local.result->outcome.error().is(shell::ErrorCode::Cancelled),
          "local owner receives cancellation result");
    // A client that stops reading must not hold the network worker or its script forever.
    Client stalled(server.endpoint(), stalled_receive_buffer_bytes);
    auto flood = files.write("flood.tcl", "while {1} {puts [string repeat x 65536]}\n");
    stalled.send("srun " + shell::quote_argument(text(flood)) + "\n");
    stalled.until("Script running.");
    check(observer_client.command("help").find("[Commands]") != std::string::npos,
          "stalled peer cannot block another connection");
    check(eventually([&] {
              return !scripts.active();
          }),
          "send stall disconnects and expiry cancels flood script");
    server.begin_shutdown();
    server.finish_shutdown();
}

void test_connection_capacity() {
    constexpr int expected_default_connections = 8;
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream local_output;
    auto local_owner = checked_value(vinc::Shell::create(manager, local_output));
    auto& local = *local_owner;
    shell::SocketShellOptions options;
    options.port = 0;
    auto address = checked_value(local.start_socket_shell(options));
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < expected_default_connections; ++i) {
        auto client = std::make_unique<Client>(address);
        check(client->command("output on").find("Module output on") != std::string::npos,
              "eight remote output subscriptions plus local subscription");
        clients.push_back(std::move(client));
    }

    auto ninth = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    take(ninth.connect_until(address, Clock::now() + capacity_probe_timeout));
    std::array<std::byte, 64> bytes{};
    auto received = ninth.receive_until(bytes, Clock::now() + capacity_probe_timeout);
    check((received && received->eof) || (!received && !received.error().is(vinc::osal::ErrorCode::Timeout)),
          "ninth connection is closed at capacity");
    local.wait_shutdown();
}

void test_ipv6_listener() {
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    shell::ScriptService scripts(manager);
    check(scripts.initialize(), "initialize script service");
    shell::SocketShellOptions options;
    options.ip = "::1";
    options.port = 0;
    auto server_owner = checked_value(shell::SocketShell::create(manager, scripts, options));
    auto& server = *server_owner;
    Client client(server.endpoint());
    check(client.command("help").find("[Commands]") != std::string::npos, "configured IPv6 listener");
}

void test_remote_shutdown(Files& files, char** argv) {
    // Full local event loop: remote exit must wait for the actual child stop hook.
    auto manager_owner = vinc::test::create_manager();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    InputPipe input;
    std::ostringstream output;
    auto shell_owner = checked_value(vinc::Shell::create(manager, output));
    auto& shell = *shell_owner;
    check(bool(manager.register_module_type(
              {"shellHelper", std::filesystem::absolute(argv[1]), "socketNode", {}})),
          "register module");
    check(!manager.register_module_type({"resume", std::filesystem::absolute(argv[1]), "reserved", {}}),
          "remote names reserved");
    shell::SocketShellOptions options;
    options.port = 0;
    const auto endpoint = checked_value(shell.start_socket_shell(options));
    auto loop = std::async(std::launch::async, [&] {
        check(shell.run(), "Shell run");
    });

    struct Guard {
        InputPipe& input;
        std::future<void>& loop;

        ~Guard() {
            if (loop.valid()) {
                input.send("\x03");
                loop.wait();
            }
        }
    } guard{input, loop};

    Client owner_client(endpoint);
    Client observer_client(endpoint);
    observer_client.command("output off");
    owner_client.send("shellHelper output " + shell::quote_argument(text(files.path)) + "\n");
    owner_client.until("Started socketNode");
    owner_client.until("VINC > ");
    check(eventually([&] {
              return std::filesystem::exists(files.path / "socketNode.ready");
          }),
          "remote starts real process");
    owner_client.send("socketNode output\n");
    owner_client.until("command-output\\x1B[2J");
    owner_client.pending.clear();
    auto result = observer_client.command("help");
    check(result.find("command-output") == std::string::npos,
          "muted module output does not hide command reply");
    owner_client.send("socketNode slow\nsocketNode slow\n");
    owner_client.until("Busy:");
    check(observer_client.command("help").find("[Commands]") != std::string::npos,
          "slow command does not block other clients");
    observer_client.send("exit\n");
    observer_client.until("All module processes exited; framework shutdown complete.");
    owner_client.until("stop-tail-without-newline");
    owner_client.until("All module processes exited; framework shutdown complete.");
    check(loop.wait_for(remote_shutdown_timeout) == std::future_status::ready,
          "remote exit completes framework loop");
    loop.get();
    check(std::filesystem::exists(files.path / "socketNode.stopped"),
          "module stop hook completed before framework exit");
    check(output.str().find("stop-tail-without-newline") != std::string::npos,
          "local output drained during remote shutdown");
}

void test_remote_contract(Files& files, const std::filesystem::path& helper) {
    auto manager = take(vinc::test::create_manager());
    take(manager->register_module_type({"shellHelper", helper, "faultNode", {}}));
    const auto mid = take(manager->start_module("shellHelper", {"normal", text(files.path)}));
    take(manager->wait_running(mid, Clock::now() + fixture_wait_timeout));
    shell::ScriptService scripts(*manager);
    take(scripts.initialize());
    shell::SocketShellOptions options;
    options.port = 0;
    auto server = take(shell::SocketShell::create(*manager, scripts, options));
    Client client(server->endpoint());
    client.command("output off");
    (void)manager->command(mid, {"contract"}, Clock::now() + fixture_wait_timeout);
    const auto notification = client.until("index=10 size=3");
    check(notification.find("[Module fault] faultNode MID=") != std::string::npos,
          "remote diagnostic is independent of muted module output");
    client.until("VINC > ");
    const auto help = client.command("help");
    check(help.find("[Module fault] faultNode MID=") != std::string::npos,
          "remote remains usable and can query retained fault");
    client.close();
    server.reset();
    manager->begin_shutdown();
    take(manager->wait_shutdown(Clock::now() + fixture_wait_timeout));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    try {
        check(shell::initialize_tcl(argv[0]), "initialize Tcl");
        Files files;
        std::array<std::byte, 600> random1{};
        std::array<std::byte, 600> random2{};
        take(osal::random_bytes(random1));
        take(osal::random_bytes(random2));
        check(random1 != random2, "OS random source fills multiple chunks");
        take(osal::random_bytes({}));
        test_sessions_and_backpressure(files);
        test_connection_capacity();
        test_ipv6_listener();
        test_remote_contract(files, argv[1]);
        test_remote_shutdown(files, argv);
        std::cout << "Socket Shell tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
