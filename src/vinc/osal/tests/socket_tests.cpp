// Socket contract tests across addresses, streams, datagrams and readiness polling.
// Include cancellation, peer loss and descriptor reuse so readiness is not confused with completed I/O.

#include <vinc/time.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <vinc/osal/process.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace vinc::osal;
using namespace std::chrono_literals;

namespace {
constexpr auto operation_timeout = 5s;
constexpr auto bulk_transfer_timeout = 10s;
constexpr auto receive_timeout_probe = 15ms;
constexpr auto idle_poller_timeout = 10ms;
constexpr auto async_signal_delay = 20ms;
constexpr auto backpressure_write_timeout = 20ms;
constexpr auto child_hold_duration = 30s;
constexpr int unique_path_attempts = 16;
constexpr int fairness_wait_iterations = 8;
constexpr int coalesced_wake_count = 100;
constexpr int maximum_backpressure_writes = 1024;
constexpr int lifecycle_iterations = 50;
constexpr std::size_t stream_read_chunk_bytes = 997;
constexpr std::size_t stream_payload_bytes = 1024 * 1024 + 17;
constexpr std::size_t backpressure_chunk_bytes = 65536;
constexpr int backpressure_socket_buffer_bytes = 4096;
#if defined(_WIN32)
constexpr DWORD handle_count_allowance = 4;
#endif
} // namespace

namespace {
vinc::Deadline after(std::chrono::milliseconds time = operation_timeout) {
    return std::chrono::steady_clock::now() + time;
}

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(vinc::Result<void> result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + vinc::error_to_string(result.error()));
    }
}

template <class T>
void expect(const vinc::Result<T>& result, vinc::osal::ErrorCode code, const char* message) {
    require(!result && result.error().is(code), message);
}

SocketAddress loopback(AddressFamily family) {
    if (family == AddressFamily::IPv6) {
        return IpEndpoint{IPv6Address::loopback(), 0};
    }

    return IpEndpoint{IPv4Address::loopback(), 0};
}

struct Pair {
    Socket sender;
    Socket receiver;
};

Pair connected(AddressFamily family) {
    auto listener = take(Socket::open(family, SocketType::Stream), "listener open");
    if (family == AddressFamily::IPv6) {
        take(listener.set_option<socket_options::IPv6Only>(true), "v6-only");
    }
    take(listener.bind(loopback(family)), "bind listener");
    take(listener.listen(8), "listen");
    auto sender = take(Socket::open(family, SocketType::Stream), "client open");
    take(sender.connect_until(take(listener.local_address(), "listener address"), after()), "connect");
    auto receiver = take(listener.accept_until(after()), "accept");
    return {std::move(sender), std::move(receiver)};
}

void send_all(Socket& socket, std::span<const std::byte> bytes, vinc::Deadline deadline) {
    while (!bytes.empty()) {
        const auto count = take(socket.send_until(bytes, deadline), "send all");
        require(count > 0, "nonempty send made no progress");
        bytes = bytes.subspan(count);
    }
}

struct LocalPath {
    std::filesystem::path directory;
    std::string path;

    LocalPath() {
        std::random_device random;
        for (int i = 0; i < unique_path_attempts; ++i) {
            auto candidate = std::filesystem::current_path() / ("s-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory = candidate;
                break;
            }
        }
        require(!directory.empty(), "unique socket directory");
        auto encoded = (directory / "s").u8string();
        path.assign(encoded.begin(), encoded.end());
    }

    ~LocalPath() {
        // Only the path and directory this fixture created; no recursive delete.
#if defined(_WIN32)
        DeleteFileW((directory / "s").c_str());
        RemoveDirectoryW(directory.c_str());
#else
        std::error_code ignored;
        std::filesystem::remove(directory / "s", ignored);
        std::filesystem::remove(directory, ignored);
#endif
    }
};

void addresses() {
    auto v4 = take(IPv4Address::parse("127.0.0.1"), "parse v4");
    require(v4 == IPv4Address::loopback() && v4.to_string() == "127.0.0.1", "v4 roundtrip");
    auto v6 = take(IPv6Address::parse("fe80::1", 7), "parse v6");
    require(v6.scope_id == 7 && take(IPv6Address::parse(v6.to_string(), 7), "v6 roundtrip") == v6,
            "v6 scope");
    expect(IPv4Address::parse("256.1.2.3"), vinc::osal::ErrorCode::InvalidArgument, "invalid v4");
    expect(IPv6Address::parse("localhost"), vinc::osal::ErrorCode::InvalidArgument,
           "parse must not resolve DNS");
    expect(IPv4Address::parse(std::string_view("127.0.0.1\0junk", 14)),
           vinc::osal::ErrorCode::InvalidArgument, "embedded NUL");
    auto resolved =
        take(resolve_host("localhost", 4321, SocketType::Stream, AddressFamily::IPv4), "resolver");
    require(!resolved.empty() && resolved[0].port == 4321, "resolver port");
    expect(resolve_host("localhost", 0, SocketType::Stream, AddressFamily::Local),
           vinc::osal::ErrorCode::InvalidArgument, "local DNS rejected");
}

void stream(AddressFamily family) {
    auto pair = connected(family);
    require(!take(pair.sender.take_pending_error(), "no pending error"),
            "fresh connection has no native error");
    std::array<std::byte, stream_read_chunk_bytes> buffer;
    expect(pair.receiver.try_receive(buffer), vinc::osal::ErrorCode::WouldBlock,
           "empty stream is WouldBlock");
    expect(pair.receiver.try_receive({}), vinc::osal::ErrorCode::InvalidArgument,
           "empty stream receive buffer");
    expect(pair.receiver.receive_until(buffer, after(receive_timeout_probe)), vinc::osal::ErrorCode::Timeout,
           "read timeout");
    std::vector<std::byte> payload(stream_payload_bytes);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((i * 37) % 251);
    }

    std::exception_ptr failure;
    std::jthread sender([&] {
        try {
            send_all(pair.sender, payload, after(bulk_transfer_timeout));
            take(pair.sender.shutdown(ShutdownDirection::Write), "half-close sender");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::size_t offset = 0;
    for (;;) {
        auto received =
            take(pair.receiver.receive_until(buffer, after(bulk_transfer_timeout)), "stream receive");
        if (received.eof) {
            break;
        }
        require(!received.truncated && received.bytes > 0 && offset + received.bytes <= payload.size(),
                "stream progress");
        require(std::equal(buffer.begin(), buffer.begin() + received.bytes, payload.begin() + offset),
                "binary stream integrity");
        offset += received.bytes;
    }
    sender.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    require(offset == payload.size(), "EOF follows all buffered bytes");
    std::array<std::byte, 3> reply{std::byte{1}, std::byte{2}, std::byte{3}};
    send_all(pair.receiver, reply, after());
    auto received = take(pair.sender.receive_until(buffer, after()), "reverse direction after half-close");
    require(received.bytes == reply.size() && !received.eof, "half-close preserves reverse direction");
    auto moved = std::move(pair.receiver);
    require(!pair.receiver && moved, "move transfers ownership");
    take(moved.close(), "close");
    take(moved.close(), "repeat close");
    expect(moved.try_receive(buffer), vinc::osal::ErrorCode::Closed, "closed socket rejected");
}

void datagrams(AddressFamily family) {
    auto sender = take(Socket::open(family, SocketType::Datagram), "UDP sender");
    auto receiver = take(Socket::open(family, SocketType::Datagram), "UDP receiver");
    take(sender.bind(loopback(family)), "UDP sender bind");
    take(receiver.bind(loopback(family)), "UDP receiver bind");
    auto target = take(receiver.local_address(), "UDP target");
    auto source = take(sender.local_address(), "UDP source");
    take(sender.send_to_until({}, target, after()), "empty datagram send");
    std::array<std::byte, 5> small;
    auto empty = take(receiver.receive_from_until(small, after()), "empty datagram receive");
    require(empty.data.bytes == 0 && !empty.data.eof && !empty.data.truncated && empty.sender == source,
            "empty datagram is not EOF");
    std::array<std::byte, 32> message{};
    message[0] = std::byte{42};
    take(sender.send_to_until(message, target, after()), "large datagram");
    auto short_read = take(receiver.receive_from_until(small, after()), "truncated datagram");
    require(short_read.data.truncated && short_read.data.bytes == small.size() && small[0] == message[0],
            "truncation visible");
    expect(receiver.try_receive_from(small), vinc::osal::ErrorCode::WouldBlock, "datagram tail discarded");
    take(sender.send_to_until({}, target, after()), "zero datagram to empty buffer");
    require(take(receiver.receive_from_until({}, after()), "empty receive buffer").data.bytes == 0,
            "zero-length UDP valid");
    require(take(sender.try_connect(target), "connect UDP") == ConnectProgress::Connected, "UDP association");
    take(sender.send_until(message, after()), "connected UDP send");
    std::array<std::byte, 64> large;
    require(take(receiver.receive_until(large, after()), "UDP generic receive").bytes == message.size(),
            "UDP boundary");
    take(receiver.try_connect(source), "receiver association");
    take(sender.send_until(message, after()), "connected truncated UDP send");
    require(take(receiver.receive_until(small, after()), "connected truncated UDP receive").truncated,
            "connected UDP truncation");
    auto automatic = take(Socket::open(family, SocketType::Datagram), "implicitly bound UDP");
    auto sink = take(Socket::open(family, SocketType::Datagram), "automatic bind receiver");
    take(sink.bind(loopback(family)), "automatic receiver bind");
    take(automatic.send_to_until(message, take(sink.local_address(), "automatic target"), after()),
         "send without explicit bind");
    require(take(sink.receive_from_until(large, after()), "implicitly bound sender data").data.bytes ==
                message.size(),
            "automatic sender delivered datagram");
    auto automatic_address = take(automatic.local_address(), "automatic local address");
    require(std::get<IpEndpoint>(automatic_address).port != 0, "send assigns a local port");
    if (family == AddressFamily::IPv6) {
        expect(automatic.set_option<socket_options::IPv6Only>(false), vinc::osal::ErrorCode::InvalidArgument,
               "cannot change v6-only after implicit bind");
    }
}

void poller() {
    auto pair = connected(AddressFamily::IPv4);
    auto poller = take(SocketPoller::create(2), "poller create");
    auto first = take(poller.add(pair.receiver, SocketEvents::Readable, 123), "register receiver");
    expect(poller.add(pair.receiver, SocketEvents::Readable), vinc::osal::ErrorCode::Busy,
           "duplicate registration");
    std::array<SocketReadyEvent, 4> events;
    expect(poller.wait_until(events, after(idle_poller_timeout)), vinc::osal::ErrorCode::Timeout,
           "poller idle timeout");
    std::array<std::byte, 4> bytes{};
    send_all(pair.sender, bytes, after());
    auto count = take(poller.wait_until(events, after()), "readable event");
    require(count && events[0].registration == first && events[0].user_tag == 123 &&
                has_event(events[0].events, SocketEvents::Readable),
            "event identity");
    require(take(poller.wait_until(events, after()), "level-trigger event") > 0,
            "level trigger until drained");
    take(pair.receiver.try_receive(bytes), "drain");
    take(poller.modify(first, SocketEvents::Writable), "modify interest");
    auto second = take(poller.add(pair.sender, SocketEvents::Writable, 456), "second writable");
    auto extra = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "capacity extra");
    expect(poller.add(extra, SocketEvents::Readable), vinc::osal::ErrorCode::ResourceExhausted,
           "bounded registrations");
    std::array<bool, 2> seen{};
    for (int i = 0; i < fairness_wait_iterations; ++i) {
        SocketReadyEvent event;
        take(poller.wait_until({&event, 1}, after()), "fairness wait");
        if (event.registration == first) {
            seen[0] = true;
        }

        if (event.registration == second) {
            seen[1] = true;
        }
    }
    require(seen[0] && seen[1], "small output must not starve later sockets");
    take(poller.remove(first), "remove first");
    take(poller.remove(second), "remove second");
    expect(poller.modify(first, SocketEvents::Readable), vinc::osal::ErrorCode::NotFound,
           "stale registration rejected");
    for (int i = 0; i < coalesced_wake_count; ++i) {
        take(poller.wake(), "coalesced wake");
    }
    require(take(poller.wait_until(events, after()), "consume wake") == 0, "wake has no socket event");
    expect(poller.wait_until(events, after(idle_poller_timeout)), vinc::osal::ErrorCode::Timeout,
           "wake fully consumed");
    std::jthread wake([&] {
        std::this_thread::sleep_for(async_signal_delay);
        take(poller.wake(), "cross-thread wake");
    });
    require(take(poller.wait_until(events, after()), "wait cross-thread wake") == 0,
            "cross-thread wake result");
    wake.join();
    std::stop_source stop;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(async_signal_delay);
        stop.request_stop();
    });
    expect(poller.wait_until(events, vinc::Deadline::max(), stop.get_token()),
           vinc::osal::ErrorCode::Cancelled, "cancel infinite wait");
    cancel.join();
    // Destruction without remove is contract misuse, but must never redirect a
    // stale event to a newly allocated socket with a reused native descriptor.
    auto abandoned = take(Socket::open(AddressFamily::IPv4, SocketType::Datagram), "abandoned socket");
    take(abandoned.bind(loopback(AddressFamily::IPv4)), "abandoned bind");
    auto stale = take(poller.add(abandoned, SocketEvents::Writable), "abandoned add");
    take(abandoned.close(), "abandoned close");
    auto new_id = take(poller.add(extra, SocketEvents::Readable), "register after stale close");
    require(new_id != stale, "registration identity never reused");
    expect(poller.remove(stale), vinc::osal::ErrorCode::NotFound, "stale entry pruned");
    take(poller.remove(new_id), "remove extra");
}

void cancellations_and_failures() {
    auto pair = connected(AddressFamily::IPv4);
    std::array<std::byte, 16> bytes;
    std::stop_source source;
    std::jthread stop([&] {
        std::this_thread::sleep_for(async_signal_delay);
        source.request_stop();
    });
    expect(pair.receiver.receive_until(bytes, after(), source.get_token()), vinc::osal::ErrorCode::Cancelled,
           "cancel receive wait");
    stop.join();
    send_all(pair.sender, bytes, after());
    require(take(pair.receiver.receive_until(bytes, after()), "socket usable after cancelled read").bytes > 0,
            "cancel did not close socket");
    auto blocked = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "refusal target");
    take(blocked.bind(loopback(AddressFamily::IPv4)), "reserve unlistened port");
    auto client = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "refused client");
    auto failed = client.connect_until(take(blocked.local_address(), "refusal address"), after());
    expect(failed, vinc::osal::ErrorCode::ConnectionRefused,
           "connect reports refusal, not success on writable");
    require(!client, "failed connect_until releases socket");
    client = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "cancelled connect socket");
    expect(client.connect_until(loopback(AddressFamily::IPv4), after(), source.get_token()),
           vinc::osal::ErrorCode::Cancelled, "pre-cancel connect");
    require(!client, "cancel connect closes socket");
    auto old_address = take(pair.sender.peer_address(), "existing peer");
    expect(pair.sender.connect_until(old_address, after()), vinc::osal::ErrorCode::InvalidArgument,
           "no second connect on live stream");
    require(static_cast<bool>(pair.sender), "invalid connect must not destroy existing connection");
    take(pair.sender.set_option<socket_options::SendBuffer>(backpressure_socket_buffer_bytes),
         "bound send buffer");
    take(pair.receiver.set_option<socket_options::ReceiveBuffer>(backpressure_socket_buffer_bytes),
         "bound receive buffer");
    std::array<std::byte, backpressure_chunk_bytes> bulk{};
    bool timed_out = false;
    for (int i = 0; i < maximum_backpressure_writes; ++i) {
        auto sent = pair.sender.send_until(bulk, after(backpressure_write_timeout));
        if (!sent) {
            expect(sent, vinc::osal::ErrorCode::Timeout, "write backpressure timeout");
            timed_out = true;
            break;
        }
    }
    require(timed_out, "non-reading peer must eventually apply backpressure");
}

void nonblocking_connect() {
    auto listener = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "manual listener");
    take(listener.bind(loopback(AddressFamily::IPv4)), "manual bind");
    take(listener.listen(1), "manual listen");
    auto client = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "manual client");
    auto address = take(listener.local_address(), "manual address");
    auto progress = take(client.try_connect(address), "try connect");
    auto poller = take(SocketPoller::create(1), "manual poller");
    auto id = take(poller.add(client, SocketEvents::Writable), "manual register");
    while (progress == ConnectProgress::InProgress) {
        expect(client.take_pending_error(), vinc::osal::ErrorCode::Busy,
               "diagnostics must not consume connect error");
        expect(client.try_connect(address), vinc::osal::ErrorCode::Busy, "second connect while pending");
        SocketReadyEvent event;
        take(poller.wait_until({&event, 1}, after()), "manual wait");
        progress = take(client.finish_connect(), "finish connect");
    }

    auto peer = take(listener.accept_until(after()), "manual accept");
    take(poller.remove(id), "manual unregister");
    require(progress == ConnectProgress::Connected, "manual connect completed");
    std::stop_source stop;
    stop.request_stop();
    expect(SocketPoller::create(1, after(), stop.get_token()), vinc::osal::ErrorCode::Cancelled,
           "cancel poller initialization");
    expect(SocketPoller::create(1, vinc::Deadline::min()), vinc::osal::ErrorCode::Timeout,
           "expired poller initialization");
}

void options() {
    auto tcp = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "options TCP");
    take(tcp.set_option<socket_options::NoDelay>(true), "set no-delay");
    require(take(tcp.get_option<socket_options::NoDelay>(), "get no-delay"), "no-delay enabled");
    take(tcp.set_option<socket_options::KeepAlive>(true), "keepalive");
    require(take(tcp.get_option<socket_options::KeepAlive>(), "get keepalive"), "keepalive enabled");
    take(tcp.set_option<socket_options::SendBuffer>(16384), "send buffer");
    require(take(tcp.get_option<socket_options::SendBuffer>(), "get buffer") > 0, "effective buffer");
    expect(tcp.set_option<socket_options::ReceiveBuffer>(0), vinc::osal::ErrorCode::InvalidArgument,
           "invalid buffer size");
#if defined(_WIN32)
    expect(tcp.set_option<socket_options::ReuseAddress>(true), vinc::osal::ErrorCode::Unsupported,
           "no false SO_REUSEADDR equivalence");
    take(tcp.set_option<socket_options::ExclusiveAddressUse>(true), "Windows exclusive bind");
#else
    take(tcp.set_option<socket_options::ReuseAddress>(true), "POSIX reuse");
    expect(tcp.set_option<socket_options::ExclusiveAddressUse>(true), vinc::osal::ErrorCode::Unsupported,
           "exclusive extension");
#endif
    auto udp = take(Socket::open(AddressFamily::IPv4, SocketType::Datagram), "options UDP");
    expect(udp.set_option<socket_options::NoDelay>(true), vinc::osal::ErrorCode::Unsupported,
           "TCP option not UDP");
    take(udp.set_option<socket_options::Broadcast>(true), "broadcast");
    take(udp.set_option<socket_options::MulticastLoop>(true), "multicast loop");
    take(udp.set_option<socket_options::MulticastHops>(3), "multicast hops");
    require(take(udp.get_option<socket_options::MulticastHops>(), "get multicast hops") == 3,
            "multicast hops roundtrip");
    take(udp.set_multicast_interface(IPv4Address::loopback()), "multicast interface");
    require(take(udp.multicast_interface_v4(), "get multicast interface") == IPv4Address::loopback(),
            "multicast interface roundtrip");
    take(udp.bind(loopback(AddressFamily::IPv4)), "membership bind");
    const auto group = take(IPv4Address::parse("239.255.42.77"), "group");
    take(udp.join_multicast(group, IPv4Address::loopback()), "join multicast");
    take(udp.leave_multicast(group, IPv4Address::loopback()), "leave multicast");
    expect(udp.join_multicast(IPv4Address::loopback()), vinc::osal::ErrorCode::InvalidArgument,
           "not a multicast group");
}

void local_stream(const std::filesystem::path& executable) {
    LocalPath local;
    auto listener = take(Socket::open(AddressFamily::Local, SocketType::Stream), "local listener");
    take(listener.bind(LocalEndpoint{local.path}), "local bind");
    take(listener.listen(4), "local listen");
    require(take(listener.local_address(), "local address") == SocketAddress(LocalEndpoint{local.path}),
            "local path roundtrip");
    auto collision = take(Socket::open(AddressFamily::Local, SocketType::Stream), "collision socket");
    require(!collision.bind(LocalEndpoint{local.path}), "binding must not remove existing path");
    auto processes = take(ProcessService::create(), "local test processes");
    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--child", local.path};
    auto child = take(processes.spawn(options), "spawn real IPC peer");
    try {
        auto peer = take(listener.accept_until(after()), "local accept readiness");
        std::array<std::byte, 7> bytes{std::byte{13}, std::byte{0}, std::byte{255}};
        send_all(peer, bytes, after());
        take(peer.shutdown(ShutdownDirection::Write), "local half-close");
        std::array<std::byte, 7> returned{};
        std::size_t received = 0;
        while (received < returned.size()) {
            auto part = take(peer.receive_until(std::span(returned).subspan(received), after()),
                             "local echoed bytes");
            require(!part.eof && part.bytes > 0, "local echo premature EOF");
            received += part.bytes;
        }
        require(returned == bytes, "local process data integrity");
        auto exit = take(child.wait_until(after()), "child exit");
        require(exit.kind == ExitKind::Code && exit.value == 0, "child success");
        take(peer.close(), "close local peer");
        take(processes.shutdown_until(after()), "process shutdown");
    } catch (...) {
        (void)child.terminate();
        (void)child.wait_until(after());
        throw;
    }
    take(listener.close(), "local close");
    bool path_remains = false;
    // Enumerate the directory entry rather than following Windows' AF_UNIX
    // reparse point, which filesystem::exists may reject as an unknown target.
    for (const auto& entry : std::filesystem::directory_iterator(local.directory)) {
        path_remains |= entry.path().filename() == "s";
    }
    require(path_remains, "close must not secretly remove pathname");
    auto invalid = take(Socket::open(AddressFamily::Local, SocketType::Stream), "invalid local");
    expect(invalid.bind(LocalEndpoint{std::string(1000, 'x')}), vinc::osal::ErrorCode::InvalidArgument,
           "long local path");
    expect(invalid.bind(LocalEndpoint{}), vinc::osal::ErrorCode::InvalidArgument, "unnamed bind rejected");
    expect(invalid.bind(LocalEndpoint{std::string("\xc0\x80", 2)}), vinc::osal::ErrorCode::InvalidArgument,
           "invalid UTF-8 path");
    // Filesystem cleanup belongs to the owner of the pathname, not Socket.
    require(std::filesystem::remove(local.directory / "s"), "explicit portable path cleanup");
#if defined(_WIN32)
    expect(Socket::open(AddressFamily::Local, SocketType::Datagram), vinc::osal::ErrorCode::Unsupported,
           "local datagram capability");
#endif
}

void unicode_local_path() {
    LocalPath local;
    const auto path = local.directory / std::filesystem::path(u8"端");
    const auto encoded = path.u8string();
    const LocalEndpoint endpoint{std::string(encoded.begin(), encoded.end())};
    try {
        auto listener = take(Socket::open(AddressFamily::Local, SocketType::Stream), "unicode listener");
        take(listener.bind(endpoint), "unicode bind");
        take(listener.listen(1), "unicode listen");
        auto client = take(Socket::open(AddressFamily::Local, SocketType::Stream), "unicode client");
        take(client.connect_until(endpoint, after()), "unicode connect");
        auto peer = take(listener.accept_until(after()), "unicode accept");
        require(take(listener.local_address(), "unicode query") == SocketAddress{endpoint},
                "UTF-8 address roundtrip");
        take(client.close(), "unicode client close");
        take(peer.close(), "unicode peer close");
        take(listener.close(), "unicode listener close");
        require(std::filesystem::remove(path), "unicode path cleanup");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}

void killed_peer(const std::filesystem::path& executable) {
    LocalPath local;
    auto listener = take(Socket::open(AddressFamily::Local, SocketType::Stream), "kill listener");
    take(listener.bind(LocalEndpoint{local.path}), "kill bind");
    take(listener.listen(1), "kill listen");
    auto processes = take(ProcessService::create(), "kill process service");
    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--hold", local.path};
    auto child = take(processes.spawn(options), "kill spawn");
    try {
        auto peer = take(listener.accept_until(after()), "kill accept");
        take(child.terminate(), "terminate socket peer");
        take(child.wait_until(after()), "reap socket peer");
        std::array<std::byte, 16> bytes;
        auto result = peer.receive_until(bytes, after());
        require((result && result->eof) ||
                    (!result && result.error().is(vinc::osal::ErrorCode::ConnectionReset)),
                "dead peer must not leave a live endpoint copy");
        take(processes.shutdown_until(after()), "kill process shutdown");
    } catch (...) {
        (void)child.terminate();
        (void)child.wait_until(after());
        throw;
    }
}

void resources() {
    {
        auto warm = connected(AddressFamily::IPv4);
    }
#if defined(_WIN32)
    DWORD before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial handles");
#endif
    for (int i = 0; i < lifecycle_iterations; ++i) {
        auto pair = connected(AddressFamily::IPv4);
        auto poller = take(SocketPoller::create(), "churn poller");
        auto id = take(poller.add(pair.sender, SocketEvents::Writable), "churn add");
        SocketReadyEvent event;
        take(poller.wait_until({&event, 1}, after()), "churn wait");
        take(poller.remove(id), "churn remove");
    }
#if defined(_WIN32)
    DWORD after_count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &after_count) != FALSE, "final handles");
    require(after_count <= before + handle_count_allowance, "native handles accumulate");
#endif
}
}

int main(int argc, char** argv) {
    try {
#if !defined(_WIN32)
        if (argc == 2 && std::string_view(argv[1]) == "--closed-stdio") {
            for (int fd = 0; fd < 3; ++fd) {
                ::close(fd);
            }

            auto socket = take(Socket::open(AddressFamily::IPv4, SocketType::Stream), "closed stdio socket");
            auto poller = take(SocketPoller::create(), "closed stdio poller");
            for (int fd = 0; fd < 3; ++fd) {
                require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "internal descriptor became stdio");
            }

            return 0;
        }
#endif
        if (argc == 3 && (std::string_view(argv[1]) == "--child" || std::string_view(argv[1]) == "--hold")) {
            auto socket = take(Socket::open(AddressFamily::Local, SocketType::Stream), "child local socket");
            take(socket.connect_until(LocalEndpoint{argv[2]}, after()), "child local connect");
            if (std::string_view(argv[1]) == "--hold") {
                std::this_thread::sleep_for(child_hold_duration);
                return 0;
            }

            std::array<std::byte, 3> bytes;
            for (;;) {
                auto received = take(socket.receive_until(bytes, after()), "child receive");
                if (received.eof) {
                    return 0;
                }
                send_all(socket, std::span(bytes).first(received.bytes), after());
            }
        }
        addresses();
        std::cout << "PASS: numeric addresses, scopes, explicit resolver\n";
        stream(AddressFamily::IPv4);
        std::cout << "PASS: IPv4 stream, partial transfers, EOF, half-close, ownership\n";
        stream(AddressFamily::IPv6);
        std::cout << "PASS: IPv6 stream and half-close\n";
        datagrams(AddressFamily::IPv4);
        std::cout << "PASS: IPv4 datagram boundaries, empty messages, truncation\n";
        datagrams(AddressFamily::IPv6);
        std::cout << "PASS: IPv6 datagram boundaries and truncation\n";
        poller();
        std::cout << "PASS: level readiness, fairness, bounds, wake, cancellation, stale IDs\n";
        cancellations_and_failures();
        std::cout << "PASS: cancelled receive, refused/cancelled connect, bounded write wait\n";
        nonblocking_connect();
        std::cout << "PASS: explicit nonblocking connect and cancellable poller setup\n";
        options();
        std::cout << "PASS: typed options, binding policies, multicast membership\n";
        local_stream(std::filesystem::absolute(argv[0]));
        std::cout << "PASS: real child process AF_UNIX stream and path ownership\n";
        unicode_local_path();
        std::cout << "PASS: UTF-8 local pathname and explicit filesystem cleanup\n";
        killed_peer(std::filesystem::absolute(argv[0]));
        std::cout << "PASS: forced peer termination and endpoint collection\n";
        resources();
        std::cout << "PASS: repeated socket and poller resource collection\n";
#if !defined(_WIN32)
        auto processes = take(ProcessService::create(), "closed stdio service");
        SpawnOptions options;
        options.executable = std::filesystem::absolute(argv[0]);
        options.arguments = {"--closed-stdio"};
        auto child = take(processes.spawn(options), "closed stdio helper");
        auto exit = child.wait_until(after());
        if (!exit) {
            (void)child.terminate();
            (void)child.wait_until(after());
        }
        require(exit && exit->kind == ExitKind::Code && exit->value == 0, "closed stdio isolation");
        take(processes.shutdown_until(after()), "closed stdio shutdown");
        std::cout << "PASS: internal descriptors do not occupy missing stdio\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
