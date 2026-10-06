// Check public composition boundaries using real sockets and capacity failures.
// Only unknown-code/truncation checks exercise the private helper directly.
#include <vinc/osal/detail/operation_error.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto operation_timeout = 5s;
constexpr auto timeout_probe = 20ms;
constexpr int listener_backlog = 1;
constexpr int retry_count = 3;

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

template <class T>
void expect_chain(const vinc::Result<T>& result, ErrorCode code, ErrorOperation outer, ErrorOperation inner) {
    require(!result, "expected failure");
    const auto& error = result.error();
    require(error.is(code) && error.is_operation(outer), "outer identity changed");
    require(error.causes().size() == 1 && !error.truncated(), "cause duplicated or lost");
    const auto& cause = error.causes().front();
    require(cause.domain == vinc::ErrorDomain::Osal && cause.code == error.code &&
                cause.operation == static_cast<std::int32_t>(inner) && cause.native == error.native,
            "cause identity or native provenance changed");
}

void helper_contract() {
    constexpr std::int32_t unknown_code = 123456;
    const auto root = vinc::Error::from_raw(vinc::ErrorDomain::Osal, unknown_code,
        static_cast<std::int32_t>(ErrorOperation::RandomBytes),
        {static_cast<vinc::NativeErrorSource>(987), -321});
    auto nested = detail::operation_error(ErrorOperation::CreatePipe, root);
    require(nested.code == unknown_code && nested.native == root.native, "unknown values changed");
    require(nested.causes().size() == 1 && nested.causes().front() == root.record(), "root changed");

    // Alternate distinct operations to exceed any configured chain capacity.
    for (std::size_t index = 0; index < vinc::config::error_chain_max_depth; ++index) {
        const auto operation = index % 2 == 0 ? ErrorOperation::Spawn : ErrorOperation::CreatePipe;
        nested = detail::operation_error(operation, nested);
    }
    require(nested.truncated() && nested.causes().back() == root.record(), "deepest cause lost");
    const auto forwarded = detail::operation_error(static_cast<ErrorOperation>(nested.operation), nested);
    require(forwarded == nested, "same operation consumed a cause slot");
}

void socket_boundaries() {
    const SocketAddress loopback = IpEndpoint{IPv4Address::loopback(), 0};
    auto listener = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    take(listener.bind(loopback));
    take(listener.listen(listener_backlog));
    const auto endpoint = take(listener.local_address());
    std::stop_source stop;
    stop.request_stop();
    expect_chain(listener.accept_until(soon(), stop.get_token()), ErrorCode::Cancelled,
                 ErrorOperation::AcceptSocket, ErrorOperation::WaitSocket);
    expect_chain(listener.accept_until(std::chrono::steady_clock::now() + timeout_probe), ErrorCode::Timeout,
                 ErrorOperation::AcceptSocket, ErrorOperation::WaitSocket);

    auto sender = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    take(sender.connect_until(endpoint, soon()));
    auto receiver = take(listener.accept_until(soon()));
    std::array<std::byte, 1> bytes{std::byte{0x42}};
    expect_chain(sender.send_until(bytes, soon(), stop.get_token()), ErrorCode::Cancelled,
                 ErrorOperation::SendSocket, ErrorOperation::WaitSocket);
    expect_chain(receiver.receive_until(bytes, soon(), stop.get_token()), ErrorCode::Cancelled,
                 ErrorOperation::ReceiveSocket, ErrorOperation::WaitSocket);
    expect_chain(receiver.receive_until(bytes, std::chrono::steady_clock::now() + timeout_probe),
                 ErrorCode::Timeout, ErrorOperation::ReceiveSocket, ErrorOperation::WaitSocket);
    // Timeout/cancellation must leave the connected stream usable.
    require(take(sender.send_until(bytes, soon())) == bytes.size(), "send after cancellation");
    require(take(receiver.receive_until(bytes, soon())).bytes == bytes.size(), "receive after timeout");

    auto datagram = take(Socket::open(AddressFamily::IPv4, SocketType::Datagram));
    take(datagram.bind(loopback));
    expect_chain(datagram.send_to_until(bytes, endpoint, soon(), stop.get_token()), ErrorCode::Cancelled,
                 ErrorOperation::SendSocket, ErrorOperation::WaitSocket);
    expect_chain(datagram.receive_from_until(bytes, soon(), stop.get_token()), ErrorCode::Cancelled,
                 ErrorOperation::ReceiveSocket, ErrorOperation::WaitSocket);

    auto cancelled_client = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    auto cancelled = cancelled_client.connect_until(endpoint, soon(), stop.get_token());
    require(!cancelled && cancelled.error().is(ErrorCode::Cancelled) &&
                cancelled.error().is_operation(ErrorOperation::ConnectSocket) &&
                cancelled.error().causes().empty() && !cancelled_client,
            "direct connect cancellation changed");

    Socket closed;
    auto invalid = closed.receive_until(bytes, soon());
    require(!invalid && invalid.error().is(ErrorCode::Closed) &&
                invalid.error().is_operation(ErrorOperation::ReceiveSocket) && invalid.error().causes().empty(),
            "direct receive failure was wrapped again");

    auto poller = take(SocketPoller::create());
    SocketReadyEvent event;
    auto waited = poller.wait_until({&event, 1}, soon(), stop.get_token());
    require(!waited && waited.error().is_operation(ErrorOperation::WaitSocket) && waited.error().causes().empty(),
            "direct poller failure was wrapped again");
}

void spawn_boundaries(const std::filesystem::path& executable) {
    auto processes = take(ProcessService::create());
    PipeServiceOptions options;
    options.max_endpoints = 2;
    auto pipes = take(PipeService::create(options));
    SpawnOptions spawn;
    spawn.executable = executable;
    // Duplex creation needs four endpoint credits. Each failed attempt must
    // release the first pipe, so later attempts still reach the second create.
    for (int attempt = 0; attempt < retry_count; ++attempt) {
        expect_chain(spawn_with_pipe(processes, pipes, spawn), ErrorCode::ResourceExhausted,
                     ErrorOperation::Spawn, ErrorOperation::CreatePipe);
    }

    auto pipe = take(pipes.create_pipe());
    spawn.stdout_mode = StdioMode::Capture;
    expect_chain(spawn_with_output(processes, pipes, spawn), ErrorCode::ResourceExhausted,
                 ErrorOperation::Spawn, ErrorOperation::CreatePipe);

    spawn.stdout_mode = StdioMode::Inherit;
    spawn.executable.clear();
    const std::array<PipeBinding, 1> bindings{{{42, &pipe.reader}}};
    auto rejected = spawn_with_pipes(processes, pipes, spawn, bindings);
    require(!rejected && rejected.error().is(ErrorCode::InvalidArgument) &&
                rejected.error().is_operation(ErrorOperation::Spawn) && rejected.error().causes().empty(),
            "same-operation spawn failure duplicated");
    require(static_cast<bool>(pipe.reader) && static_cast<bool>(pipe.writer), "failed handoff consumed endpoints");
    take(pipes.shutdown_until(soon()));
    take(processes.shutdown_until(soon()));
}
}

int main(int argc, char** argv) {
    try {
        require(argc >= 1, "missing executable path");
        helper_contract();
        socket_boundaries();
        spawn_boundaries(std::filesystem::absolute(argv[0]));
        std::cout << "PASS: OSAL operations, bounded causes and failure cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
