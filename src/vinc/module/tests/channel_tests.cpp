#include <vinc/control/channel.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace vinc;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::pair<osal::Socket, osal::Socket> sockets() {
    auto listener = osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream); check(bool(listener), "open listener");
    check(bool(listener->bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0})), "bind");
    check(bool(listener->listen(4)), "listen");
    auto address = listener->local_address(); check(bool(address), "address");
    auto client = osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream); check(bool(client), "open client");
    check(bool(client->connect_until(*address, control::Clock::now() + 2s)), "connect");
    auto server = listener->accept_until(control::Clock::now() + 2s); check(bool(server), "accept");
    return {std::move(*server), std::move(*client)};
}
int main() {
    try {
        control::Frame frame{control::Kind::Request, 42, 7, 19, {std::byte{1}, std::byte{2}}};
        const auto wire = control::encode(frame);
        control::Decoder decoder; int count = 0;
        for (auto byte : wire) {
            auto decoded = decoder.feed(std::span<const std::byte>(&byte, 1)); check(bool(decoded), "fragment parse");
            for (const auto& message : *decoded) { ++count; check(message.request == 19 && message.domain == 42 && message.payload == frame.payload, "frame roundtrip"); }
        }
        check(count == 1, "exactly one fragmented frame");
        auto coalesced = wire; coalesced.insert(coalesced.end(), wire.begin(), wire.end());
        check(decoder.feed(coalesced)->size() == 2, "coalesced frames");
        auto invalid = wire; invalid[20] = std::byte{0x7f}; control::Decoder bad;
        check(!bad.feed(invalid), "oversize rejected before payload allocation");
        invalid = wire; invalid[5] = std::byte{2}; control::Decoder version;
        check(!version.feed(invalid), "version rejected");
        auto service = control::Service::create(4); check(bool(service), "service");
        auto [a, b] = sockets();
        control::Options central; central.generation = 1; central.token = "test";
        central.heartbeat_interval = 40ms; central.heartbeat_timeout = 400ms;
        auto child = central; child.role = control::Role::Module;
        auto left = (*service)->attach(std::move(a), central); auto right = (*service)->attach(std::move(b), child);
        check(left && right, "attach");
        check(bool((*left)->wait_open(control::Clock::now() + 2s)), "left handshake");
        check(bool((*right)->wait_open(control::Clock::now() + 2s)), "right handshake");
        check(bool((*left)->send(frame)), "send extension frame");
        auto received = (*right)->receive_until(control::Clock::now() + 2s);
        check(received && received->domain == 42 && received->request == 19, "unknown feature transported unchanged");
        std::this_thread::sleep_for(900ms);
        check((*left)->state() == control::State::Open && (*right)->state() == control::State::Open, "bidirectional heartbeat kept idle sessions alive");
        (*right)->close();
        auto disconnected = (*left)->receive_until(control::Clock::now() + 2s);
        check(!disconnected && disconnected.error().code != control::ErrorCode::Timeout, "disconnect propagation");
        auto [c, d] = sockets(); child.token = "wrong";
        auto wrong = (*service)->attach(std::move(c), central); auto other = (*service)->attach(std::move(d), child);
        check(wrong && other && !(*wrong)->wait_open(control::Clock::now() + 2s), "handshake identity mismatch");
        auto bounded_service = control::Service::create(1); check(bool(bounded_service), "bounded service");
        auto [server, raw_client] = sockets();
        auto bounded = (*bounded_service)->attach(std::move(server), central); check(bool(bounded), "bounded attach");
        control::Frame raw_hello{control::Kind::Hello};
        control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(control::Role::Module));
        control::put_u64(raw_hello.payload, central.generation); control::put_string(raw_hello.payload, central.token);
        control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_interval.count()));
        control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_timeout.count()));
        auto raw_wire = control::encode(raw_hello); std::size_t offset = 0;
        while (offset < raw_wire.size()) {
            auto sent = raw_client.send_until(std::span<const std::byte>(raw_wire).subspan(offset), control::Clock::now() + 2s);
            check(sent && *sent, "raw handshake send"); offset += *sent;
        }
        check(bool((*bounded)->wait_open(control::Clock::now() + 2s)), "raw peer handshake");
        bool full = false;
        control::Frame large{control::Kind::Event, 42, 1}; large.payload.resize(control::max_payload);
        for (int i = 0; i < 2048 && !full; ++i) {
            auto queued = (*bounded)->send(large);
            if (!queued) { check(queued.error().code == control::ErrorCode::Backpressure, "explicit queue backpressure"); full = true; }
        }
        check(full, "bounded sender refuses backlog");
        auto lost = (*bounded)->receive_until(control::Clock::now() + 2s);
        check(!lost && (*bounded)->state() == control::State::Closed && (*bounded)->error()->message == "heartbeat timeout", "silent connected peer times out despite queued traffic");
        std::cout << "control framing, sessions, heartbeat and disconnect tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
