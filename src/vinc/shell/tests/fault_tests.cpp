#include "../../../../tests/support/manager.hpp"
// Exercise actual implementation boundaries with test-only OSAL result injection.
#include "../src/socket_shell.cpp"
#include "../src/shell.cpp"
#include "input_pipe.hpp"
#include <iostream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif

using namespace vinc;
using namespace std::chrono_literals;
namespace {
constexpr auto test_timeout = 4s;
constexpr auto poll_delay = 5ms;
std::mutex injection_mutex;
std::optional<Error> accept_error;
std::atomic<bool> fail_poll{false};
bool fail_input = false;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}
template<class F> bool eventually(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(poll_delay);
    }
    return false;
}
void inject_accept(std::optional<Error> error) {
    std::lock_guard lock(injection_mutex);
    accept_error = error;
}
Error os_error(osal::ErrorCode code) {
    return Error{code, osal::ErrorOperation::AcceptSocket,
                 NativeError{NativeErrorSource::LinuxErrno, 123}};
}
osal::Socket connect(osal::IpEndpoint endpoint) {
    auto socket = take(osal::Socket::open(osal::address_family(endpoint), osal::SocketType::Stream));
    take(socket.connect_until(endpoint, std::chrono::steady_clock::now() + test_timeout));
    return socket;
}
}
namespace vinc::shell::test {
Result<osal::Socket> accept(osal::Socket& listener) {
    {
        std::lock_guard lock(injection_mutex);
        if (accept_error) {
            return std::unexpected(*accept_error);
        }
    }
    return listener.try_accept();
}
Result<std::size_t> wait(osal::SocketPoller& poller, std::span<osal::SocketReadyEvent> events,
                         vinc::Deadline deadline) {
    if (fail_poll.exchange(false)) {
        return std::unexpected(Error{osal::ErrorCode::SystemError, osal::ErrorOperation::WaitSocket});
    }
    return poller.wait_until(events, deadline);
}
}
namespace input_pipe_test {
void checkpoint() {
    if (fail_input) {
        throw std::runtime_error("injected stdin installation failure");
    }
}
}
namespace vinc::shell {
struct SocketShellTestAccess {
    static auto idle(ModuleManager& manager, ScriptService& scripts) {
        SocketShellOptions options;
        options.port = 0;
        auto impl = std::make_unique<SocketShell::Impl>(manager, scripts, options,
                        take(osal::SocketPoller::create(config::shell_socket_poller_capacity)));
        take(impl->initialize());
        return std::unique_ptr<SocketShell>(new SocketShell(std::move(impl)));
    }
    static void check_broken_poller_cleanup(SocketShell& server) {
        auto& impl = *server.impl_;
        auto client = connect(server.endpoint());
        take(impl.accept());
        // Existing registrations no longer belong to this replacement poller.
        // Both modify and cleanup/remove fail; native sockets must still close.
        impl.poller = take(osal::SocketPoller::create(config::shell_socket_poller_capacity));
        impl.run();
        check(!impl.listener && impl.sessions.empty() && !server.shutdown_requested(),
              "poller registration loss cannot leave unserviced sockets alive");
    }
    static void check_boundaries(SocketShell& server) {
        auto& impl = *server.impl_;
        SocketShell::Impl::Session queue(100, "fixture");
        queue.sending_text = "in flight";
        for (std::size_t index = 1; index < config::shell_socket_queue_items; ++index) {
            check(impl.enqueue(queue, "queued"), "admit exactly 128 total items");
        }
        check(!impl.enqueue(queue, "overflow") && queue.disconnect_requested,
              "in-flight item counts toward queue capacity");

        auto client = connect(server.endpoint());
        take(impl.accept());
        check(impl.sessions.size() == 1, "accept existing client");
        inject_accept(os_error(osal::ErrorCode::WouldBlock));
        take(impl.accept());
        check(!server.failure(), "WouldBlock has no diagnostic");
        inject_accept(os_error(osal::ErrorCode::ConnectionReset));
        take(impl.accept());
        check(server.status().failures == config::shell_socket_accept_budget && impl.listener,
              "connection failures retain listener and obey per-pass budget");
        inject_accept(os_error(osal::ErrorCode::ResourceExhausted));
        take(impl.accept());
        check(impl.listener && !impl.listener_registration.value && impl.accept_retry_at,
              "resource pressure suspends watch without closing listener");
        check(impl.sessions.front()->socket && !server.shutdown_requested(), "existing client survives");
        take(impl.register_listener());
        inject_accept(os_error(osal::ErrorCode::InvalidArgument));
        take(impl.accept());
        check(!impl.listener && impl.sessions.front()->socket && !server.shutdown_requested(),
              "listener failure preserves existing client and framework");
        const auto error = server.failure();
        check(error && error->is(shell::ErrorCode::SocketFailed) &&
                  error->is_operation(shell::ErrorOperation::Accept) && !error->causes().empty(),
              "accept retains layered diagnostics");
        inject_accept({});
    }
};
}
namespace vinc {
struct ShellTestAccess {
    static auto status(Shell& shell) {
        return shell.impl_->remote->status();
    }
};
}
namespace {
void test_input_rollback() {
    // Prime CRT exception bookkeeping before comparing native resource counts.
    fail_input = true;
    try {
        InputPipe input;
    } catch (const std::runtime_error&) {
    }
#ifdef _WIN32
    const auto original = GetStdHandle(STD_INPUT_HANDLE);
    DWORD before = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before), "count initial handles");
#else
    struct stat before{};
    check(fstat(STDIN_FILENO, &before) == 0, "inspect initial stdin");
    const auto count_descriptors = [] {
        int count = 0;
        constexpr int inspected_descriptors = 1024;
        for (int descriptor = 0; descriptor < inspected_descriptors; ++descriptor) {
            if (fcntl(descriptor, F_GETFD) >= 0) {
                ++count;
            }
        }
        return count;
    };
    const auto descriptors_before = count_descriptors();
#endif
    fail_input = true;
    constexpr int rollback_attempts = 32;
    for (int attempt = 0; attempt < rollback_attempts; ++attempt) {
        bool failed = false;
        try {
            InputPipe input;
        } catch (const std::runtime_error&) {
            failed = true;
        }
        check(failed, "inject acquisition failure");
    }
    fail_input = false;
#ifdef _WIN32
    DWORD after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &after), "count final handles");
    check(before == after && original == GetStdHandle(STD_INPUT_HANDLE), "rollback handles and stdin");
#else
    struct stat after{};
    check(fstat(STDIN_FILENO, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
              descriptors_before == count_descriptors(), "rollback descriptors and stdin");
#endif
    InputPipe succeeding;
    succeeding.send("ok\n");
}
void test_accept_backoff() {
    auto manager = take(vinc::test::create_manager());
    shell::ScriptService scripts(*manager);
    shell::SocketShellOptions options;
    options.port = 0;
    inject_accept(os_error(osal::ErrorCode::ResourceExhausted));
    auto server = take(shell::SocketShell::create(*manager, scripts, options));
    auto client = connect(server->endpoint());
    check(eventually([&] {
                  return server->status().failures == 1;
              }), "observe first resource failure");
    std::this_thread::sleep_for(config::shell_socket_accept_retry_initial / 2);
    check(server->status().failures == 1 && !server->status().accepting, "backoff does not busy-poll");
    inject_accept({});
    std::array<std::byte, 2048> bytes{};
    const auto received = take(client.receive_until(bytes, std::chrono::steady_clock::now() + test_timeout));
    check(received.bytes != 0 && server->status().running && !server->shutdown_requested(),
          "listener recovers and accepts pending connection");
    server->finish_shutdown();
}
void test_listener_isolation() {
    auto manager = take(vinc::test::create_manager());
    shell::ScriptService scripts(*manager);
    shell::SocketShellOptions options;
    options.port = 0;
    auto server = take(shell::SocketShell::create(*manager, scripts, options));
    auto client = connect(server->endpoint());
    std::array<std::byte, 4096> bytes{};
    take(client.receive_until(bytes, std::chrono::steady_clock::now() + test_timeout));
    inject_accept(os_error(osal::ErrorCode::InvalidArgument));
    auto rejected_client = connect(server->endpoint());
    check(eventually([&] {
                  return !server->status().accepting;
              }), "listener is disabled");
    inject_accept({});
    const std::string command = "help\n";
    take(client.send_until(std::as_bytes(std::span(command)), std::chrono::steady_clock::now() + test_timeout));
    std::string reply;
    check(eventually([&] {
        auto received = client.receive_until(bytes, std::chrono::steady_clock::now() + poll_delay);
        if (received && !received->eof) {
            // char reads initialized byte storage while its owner remains alive.
            reply.append(reinterpret_cast<const char*>(bytes.data()), received->bytes);
        }
        return reply.find("[Commands]") != std::string::npos;
    }), "existing client still executes commands after listener failure");
    check(server->status().running && !server->shutdown_requested(), "network worker and framework survive");
    server->finish_shutdown();
}

void test_worker_isolation(const char* helper) {
    auto manager = take(vinc::test::create_manager());
    std::ostringstream output;
    auto local = take(Shell::create(*manager, output));
    take(manager->register_module_type({"shellHelper", std::filesystem::absolute(helper), "alive", {}}));
    const auto directory = std::filesystem::current_path() /
        ("shell-faults-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct DirectoryGuard {
        std::filesystem::path path;
        ~DirectoryGuard() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } directory_guard{directory};
    const auto module = take(manager->start_module("shellHelper", {"normal", take(osal::path_to_utf8(directory))}));
    take(manager->wait_started(module));
    shell::SocketShellOptions options;
    options.port = 0;
    take(local->start_socket_shell(options));
    InputPipe input;
    std::atomic<bool> survived{false};
    std::jthread feeder([&] {
        fail_poll.store(true);
        const bool stopped = eventually([&] {
                  return !ShellTestAccess::status(*local).running;
              });
        const auto snapshot = manager->inspect(module);
        survived.store(stopped && snapshot && snapshot->state == ModuleState::Running);
        input.send("help\nexit\n");
    });
    take(local->run());
    feeder.join();
    check(survived.load(), "poller failure does not stop module");
    check(output.str().find("[Modules]") != std::string::npos &&
              output.str().find("Socket Shell diagnostic") != std::string::npos,
          "local shell remains usable and reports isolated failure");
    const auto state = manager->inspect(module);
    check(state && state->state == ModuleState::Stopped, "explicit exit still performs full shutdown");
}
}
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        take(shell::initialize_tcl(argv[0]));
        test_input_rollback();
        {
            auto manager = take(vinc::test::create_manager());
            shell::ScriptService scripts(*manager);
            auto server = shell::SocketShellTestAccess::idle(*manager, scripts);
            shell::SocketShellTestAccess::check_boundaries(*server);
            auto broken = shell::SocketShellTestAccess::idle(*manager, scripts);
            shell::SocketShellTestAccess::check_broken_poller_cleanup(*broken);
        }
        test_accept_backoff();
        test_listener_isolation();
        test_worker_isolation(argv[1]);
        std::cout << "Shell fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
