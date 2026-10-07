#pragma once

#include <vinc/control/channel.hpp>
#include <array>
#include <stdexcept>
#include <thread>

namespace channel_test {
using namespace vinc;
using namespace std::chrono_literals;
inline constexpr auto operation_timeout = 2s;
inline constexpr auto state_check_interval = 1ms;
inline constexpr std::size_t listener_backlog = 4;

inline void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

inline osal::Socket open_listener() {
    auto listener = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    check(bool(listener.bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0})), "bind");
    check(bool(listener.listen(listener_backlog)), "listen");
    return listener;
}

inline osal::Socket connect_client(osal::Socket& listener) {
    auto address = take(listener.local_address());
    auto client = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    check(bool(client.connect_until(address, control::Clock::now() + operation_timeout)), "connect");
    return client;
}

// Each helper returns an owner; failure unwinds all handles acquired so far.
inline std::pair<osal::Socket, osal::Socket> connected_sockets() {
    auto listener = open_listener();
    auto client = connect_client(listener);
    auto server = take(listener.accept_until(control::Clock::now() + operation_timeout));
    return {std::move(server), std::move(client)};
}

inline control::Options options() {
    control::Options result;
    result.generation = 7;
    result.token = "control regression";
    result.heartbeat_interval = 100ms;
    result.heartbeat_timeout = 10s;
    result.handshake_timeout = operation_timeout;
    return result;
}

inline void send_bytes(osal::Socket& socket, std::span<const std::byte> bytes) {
    const auto deadline = control::Clock::now() + operation_timeout;
    while (!bytes.empty()) {
        auto sent = take(socket.send_until(bytes, deadline));
        check(sent != 0, "raw send made no progress");
        bytes = bytes.subspan(sent);
    }
}

inline void send_frame(osal::Socket& socket, const control::Frame& frame) {
    auto bytes = take(control::encode(frame));
    send_bytes(socket, bytes);
}

inline control::Frame application(std::size_t payload_bytes = 0) {
    control::Frame frame{control::Kind::Event, 42, 1};
    frame.payload.resize(payload_bytes, std::byte{0x5a});
    return frame;
}

struct RawSession {
    osal::Socket remote;
    std::shared_ptr<control::Session> session;
    control::Decoder decoder;
};

inline RawSession attach_raw(control::Service& service, const control::Options& central,
                             bool handshake = true) {
    auto [server, remote] = connected_sockets();
    auto session = take(service.attach(std::move(server), central));
    if (handshake) {
        control::Frame hello{control::Kind::Hello};
        control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Module));
        control::put_u64(hello.payload, central.generation);
        check(bool(control::put_string(hello.payload, central.token)), "encode hello token");
        control::put_u32(hello.payload, static_cast<std::uint32_t>(central.heartbeat_interval.count()));
        control::put_u32(hello.payload, static_cast<std::uint32_t>(central.heartbeat_timeout.count()));
        control::put_u32(hello.payload, control::hello_version);
        control::put_u32(hello.payload, central.heartbeat_timeout_enabled ? 1u : 0u);
        send_frame(remote, hello);
        check(bool(session->wait_open(control::Clock::now() + operation_timeout)), "raw handshake");
    }

    return {std::move(remote), std::move(session), {}};
}

inline void wait_closed(const control::Session& session) {
    const auto deadline = control::Clock::now() + operation_timeout;
    while (session.state() != control::State::Closed && control::Clock::now() < deadline) {
        std::this_thread::sleep_for(state_check_interval);
    }
    check(session.state() == control::State::Closed, "session did not close before test deadline");
}

inline void check_send_cause(control::Session& session, const Error& cause) {
    const auto expected = wrap_error(control::ErrorCode::Closed, control::ErrorOperation::Send, cause);
    constexpr int repeated_sends = 2;
    for (int attempt = 0; attempt < repeated_sends; ++attempt) {
        auto sent = session.send(application());
        check(!sent && sent.error() == expected, "send lost or repeatedly wrapped the original failure");
    }
    check(session.error() == cause, "send modified the stored failure");
}

inline std::vector<control::Frame> read_applications(RawSession& peer, std::size_t count) {
    std::vector<control::Frame> frames;
    std::array<std::byte, control::header_size + control::max_payload> bytes;
    const auto deadline = control::Clock::now() + operation_timeout;
    while (frames.size() < count) {
        auto received = take(peer.remote.receive_until(bytes, deadline));
        check(!received.eof, "unexpected raw EOF");
        auto decoded = take(peer.decoder.feed(std::span(bytes).first(received.bytes)));
        for (auto& frame : decoded) {
            if (frame.kind == control::Kind::Event) {
                frames.push_back(std::move(frame));
            }
        }
    }
    check(frames.size() == count, "received extra application frames");
    return frames;
}
}
