// Control-channel tests for framing, handshake, two-way heartbeat and bounded queues.
// Exercise real Socket sessions without invoking application module hooks.

#include <vinc/control/channel.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace vinc;
using namespace std::chrono_literals;

namespace {
constexpr auto operation_timeout = 2s;
constexpr auto heartbeat_interval = 40ms;
constexpr auto heartbeat_timeout = 400ms;
// Observe more than two timeout windows to prove that idle peers exchange heartbeats.
constexpr auto heartbeat_observation_time = 900ms;
constexpr int max_backpressure_attempts = 2048;
constexpr std::size_t listener_backlog = 4;
constexpr std::size_t test_session_capacity = 4;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

// Each preparation helper returns an owning handle. check() failures unwind all
// handles acquired so far, including an accepted socket whose peer setup fails.
osal::Socket open_listener() {
    auto listener = osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream);
    check(bool(listener), "open listener");
    check(bool(listener->bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0})), "bind");
    check(bool(listener->listen(listener_backlog)), "listen");
    return std::move(*listener);
}

osal::Socket connect_client(osal::Socket& listener) {
    auto address = listener.local_address();
    check(bool(address), "address");

    auto client = osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream);
    check(bool(client), "open client");
    check(bool(client->connect_until(*address, control::Clock::now() + operation_timeout)), "connect");
    return std::move(*client);
}

std::pair<osal::Socket, osal::Socket> connected_sockets() {
    auto listener = open_listener();
    auto client = connect_client(listener);
    auto server = listener.accept_until(control::Clock::now() + operation_timeout);
    check(bool(server), "accept");

    return {std::move(*server), std::move(client)};
}

void test_framing(const control::Frame& frame) {
    const auto wire = control::encode(frame);
    // A fixed v1 fixture catches paired encoder/decoder changes that a roundtrip misses.
    const std::array<std::uint8_t, 26> expected_wire{0x56, 0x49, 0x4e, 0x43, // VINC
                                                     0x00, 0x01, 0x00, 0x03, // version 1, Request
                                                     0x00, 0x2a, 0x00, 0x07, // domain 42, operation 7
                                                     0x00, 0x00, 0x00, 0x00,
                                                     0x00, 0x00, 0x00, 0x13, // request 19
                                                     0x00, 0x00, 0x00, 0x02, // payload length
                                                     0x01, 0x02};
    check(wire.size() == expected_wire.size(), "v1 frame size");
    for (std::size_t i = 0; i < expected_wire.size(); ++i) {
        check(std::to_integer<std::uint8_t>(wire[i]) == expected_wire[i], "v1 wire bytes unchanged");
    }

    control::Decoder decoder;
    int count = 0;
    for (auto byte : wire) {
        auto decoded = decoder.feed(std::span<const std::byte>(&byte, 1));
        check(bool(decoded), "fragment parse");
        for (const auto& message : *decoded) {
            ++count;
            check(message.request == 19 && message.domain == 42 && message.payload == frame.payload,
                  "frame roundtrip");
        }
    }
    check(count == 1, "exactly one fragmented frame");
    auto coalesced = wire;
    coalesced.insert(coalesced.end(), wire.begin(), wire.end());
    auto joined = decoder.feed(coalesced);
    check(joined && joined->size() == 2, "coalesced frames");
    auto invalid = wire;
    invalid[20] = std::byte{0x7f};
    control::Decoder bad;
    check(!bad.feed(invalid), "oversize rejected before payload allocation");
    invalid = wire;
    invalid[5] = std::byte{2};
    control::Decoder version;
    check(!version.feed(invalid), "version rejected");
}

void test_argument_bytes() {
    // Embedded NUL and UTF-8 are byte payloads, not zero-terminated wire strings.
    const std::vector<std::string> arguments{"", std::string("a\0b", 3), "\xc3\xa9"};
    auto bytes = control::encode_arguments(arguments);
    auto decoded = control::decode_arguments(bytes);
    check(decoded && *decoded == arguments, "argument bytes preserved");

    bytes.pop_back();
    check(!control::decode_arguments(bytes), "truncated string rejected");
    bytes = control::encode_arguments(arguments);
    bytes.push_back(std::byte{0});
    check(!control::decode_arguments(bytes), "trailing argument bytes rejected");
}

void test_service_lifetime(const control::Options& central) {
    auto service = control::Service::create(1);
    check(bool(service), "lifetime service");
    auto [server, raw_client] = connected_sockets();
    auto session = (*service)->attach(std::move(server), central);
    check(bool(session), "lifetime session");

    // The raw peer sends no Hello, so the session remains in Handshaking.
    auto waiting = (*session)->wait_open(control::Clock::now());
    check(!waiting && waiting.error().code == control::ErrorCode::Timeout, "waiting deadline expires");
    check((*session)->state() == control::State::Handshaking, "waiting timeout does not close session");

    auto [extra_server, extra_client] = connected_sockets();
    auto rejected = (*service)->attach(std::move(extra_server), central);
    check(!rejected && rejected.error().code == control::ErrorCode::Backpressure,
          "full service rejects peer before publication");

    service->reset();
    check((*session)->state() == control::State::Closed, "service joins worker and closes retained session");
    auto received = (*session)->try_receive();
    check(!received && received.error().code == control::ErrorCode::Closed, "retained session reports close");
    (*session)->close(); // Expired Engine link must be harmless.
}

void test_backpressure_and_timeout(const control::Options& central) {
    auto bounded_service = control::Service::create(1);
    check(bool(bounded_service), "bounded service");
    auto [server, raw_client] = connected_sockets();
    auto bounded = (*bounded_service)->attach(std::move(server), central);
    check(bool(bounded), "bounded attach");
    control::Frame raw_hello{control::Kind::Hello};
    control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(control::Role::Module));
    control::put_u64(raw_hello.payload, central.generation);
    control::put_string(raw_hello.payload, central.token);
    control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_interval.count()));
    control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_timeout.count()));
    auto raw_wire = control::encode(raw_hello);
    std::size_t offset = 0;
    while (offset < raw_wire.size()) {
        auto sent = raw_client.send_until(std::span<const std::byte>(raw_wire).subspan(offset),
                                          control::Clock::now() + operation_timeout);
        check(sent && *sent, "raw handshake send");
        offset += *sent;
    }
    check(bool((*bounded)->wait_open(control::Clock::now() + operation_timeout)), "raw peer handshake");
    bool full = false;
    control::Frame large{control::Kind::Event, 42, 1};
    large.payload.resize(control::max_payload);
    for (int i = 0; i < max_backpressure_attempts && !full; ++i) {
        auto queued = (*bounded)->send(large);
        if (!queued) {
            check(queued.error().code == control::ErrorCode::Backpressure, "explicit queue backpressure");
            full = true;
        }
    }
    check(full, "bounded sender refuses backlog");
    auto lost = (*bounded)->receive_until(control::Clock::now() + operation_timeout);
    check(!lost && (*bounded)->state() == control::State::Closed &&
              (*bounded)->error()->message == "heartbeat timeout",
          "silent connected peer times out despite queued traffic");
}

control::Options test_live_sessions(const control::Frame& frame) {
    auto service = control::Service::create(test_session_capacity);
    check(bool(service), "service");
    auto [central_socket, module_socket] = connected_sockets();
    control::Options central;
    central.generation = 1;
    central.token = "test";
    central.heartbeat_interval = heartbeat_interval;
    central.heartbeat_timeout = heartbeat_timeout;
    auto child = central;
    child.role = control::Role::Module;
    auto left = (*service)->attach(std::move(central_socket), central);
    auto right = (*service)->attach(std::move(module_socket), child);
    check(left && right, "attach");
    check(bool((*left)->wait_open(control::Clock::now() + operation_timeout)), "left handshake");
    check(bool((*right)->wait_open(control::Clock::now() + operation_timeout)), "right handshake");
    check(bool((*left)->send(frame)), "send extension frame");
    auto received = (*right)->receive_until(control::Clock::now() + operation_timeout);
    check(received && received->domain == 42 && received->request == 19,
          "unknown feature transported unchanged");
    std::this_thread::sleep_for(heartbeat_observation_time);
    check((*left)->state() == control::State::Open && (*right)->state() == control::State::Open,
          "bidirectional heartbeat kept idle sessions alive");
    (*right)->close();
    auto disconnected = (*left)->receive_until(control::Clock::now() + operation_timeout);
    check(!disconnected && disconnected.error().code != control::ErrorCode::Timeout,
          "disconnect propagation");
    auto [mismatch_central_socket, mismatch_module_socket] = connected_sockets();
    child.token = "wrong";
    auto wrong = (*service)->attach(std::move(mismatch_central_socket), central);
    auto other = (*service)->attach(std::move(mismatch_module_socket), child);
    check(wrong && other && !(*wrong)->wait_open(control::Clock::now() + operation_timeout),
          "handshake identity mismatch");
    return central;
}

}

int main() {
    try {
        control::Frame frame{control::Kind::Request, 42, 7, 19, {std::byte{1}, std::byte{2}}};
        test_framing(frame);
        test_argument_bytes();
        auto central = test_live_sessions(frame);
        test_backpressure_and_timeout(central);
        test_service_lifetime(central);
        std::cout << "control framing, sessions, heartbeat and disconnect tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
