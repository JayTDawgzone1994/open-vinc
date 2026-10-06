// Control-channel tests for framing, handshake, two-way heartbeat and bounded queues.
// Exercise real Socket sessions without invoking application module hooks.

#include "channel_test_support.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace vinc;
using namespace std::chrono_literals;

namespace {
using namespace channel_test;
constexpr auto heartbeat_interval = 40ms;
constexpr auto heartbeat_timeout = 400ms;
// Observe more than two timeout windows to prove that idle peers exchange heartbeats.
constexpr auto heartbeat_observation_time = 900ms;
constexpr int max_backpressure_attempts = 2048;
constexpr std::size_t test_session_capacity = 4;

void test_framing(const control::Frame& frame) {
    const auto wire = take(control::encode(frame));
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
    auto bytes = take(control::encode_arguments(arguments));
    auto decoded = control::decode_arguments(bytes);
    check(decoded && *decoded == arguments, "argument bytes preserved");

    bytes.pop_back();
    check(!control::decode_arguments(bytes), "truncated string rejected");
    bytes = take(control::encode_arguments(arguments));
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
    check(!waiting && waiting.error().is(control::ErrorCode::Timeout), "waiting deadline expires");
    check((*session)->state() == control::State::Handshaking, "waiting timeout does not close session");

    auto [extra_server, extra_client] = connected_sockets();
    auto rejected = (*service)->attach(std::move(extra_server), central);
    check(!rejected && rejected.error().is(control::ErrorCode::Backpressure),
          "full service rejects peer before publication");

    service->reset();
    check((*session)->state() == control::State::Closed, "service joins worker and closes retained session");
    auto received = (*session)->try_receive();
    check(!received && received.error().is(control::ErrorCode::Closed), "retained session reports close");
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
    check(bool(control::put_string(raw_hello.payload, central.token)), "encode hello token");
    control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_interval.count()));
    control::put_u32(raw_hello.payload, static_cast<std::uint32_t>(central.heartbeat_timeout.count()));
    control::put_u32(raw_hello.payload, control::hello_version);
    control::put_u32(raw_hello.payload, central.heartbeat_timeout_enabled ? 1u : 0u);
    auto raw_wire = take(control::encode(raw_hello));
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
            check(queued.error().is(control::ErrorCode::Backpressure), "explicit queue backpressure");
            full = true;
        }
    }
    check(full, "bounded sender refuses backlog");
    auto lost = (*bounded)->receive_until(control::Clock::now() + operation_timeout);
    check(!lost && (*bounded)->state() == control::State::Closed &&
              (*bounded)->error()->is(control::ErrorCode::HeartbeatTimeout),
          "silent connected peer times out despite queued traffic");
    check_send_cause(**bounded, *(*bounded)->error());
}

void test_handshake_timeout_and_normal_close() {
    auto service = take(control::Service::create(4));
    auto central = options();
    constexpr auto short_handshake_timeout = 60ms;
    central.handshake_timeout = short_handshake_timeout;
    auto peer = attach_raw(*service, central, false);
    auto early_send = peer.session->send(application());
    check(!early_send && early_send.error().is(control::ErrorCode::Closed) &&
              early_send.error().causes().empty(),
          "sending before handshake changed its error contract");
    auto opened = peer.session->wait_open(control::Clock::now() + operation_timeout);
    check(!opened && opened.error().is(control::ErrorCode::HandshakeTimeout) &&
              opened.error().is_operation(control::ErrorOperation::Handshake),
          "missing Hello did not produce HandshakeTimeout");
    check_send_cause(*peer.session, opened.error());

    auto closed = attach_raw(*service, options());
    closed.session->close();
    auto sent = closed.session->send(application());
    check(!sent && sent.error() == Error{control::ErrorCode::Closed, control::ErrorOperation::Send},
          "explicit close gained a redundant cause");
    wait_closed(*closed.session);

    auto retained = attach_raw(*service, options());
    service.reset();
    auto after_service = retained.session->send(application());
    check(!after_service && after_service.error() == sent.error(), "service shutdown gained a redundant cause");
}

void test_bad_heartbeat_isolation() {
    auto service = take(control::Service::create(8));
    auto central = options();
    auto module = central;
    module.role = control::Role::Module;
    auto [server, client] = connected_sockets();
    auto healthy_sender = take(service->attach(std::move(server), central));
    auto healthy_receiver = take(service->attach(std::move(client), module));
    check(bool(healthy_sender->wait_open(control::Clock::now() + operation_timeout)), "healthy sender open");
    check(bool(healthy_receiver->wait_open(control::Clock::now() + operation_timeout)), "healthy receiver open");

    // First zero, duplicate and decreasing sequence are all invalid. Each case
    // gets a fresh session so the first failure cannot mask the next scenario.
    const std::array<std::array<std::uint64_t, 2>, 3> sequences{{{0, 0}, {1, 1}, {2, 1}}};
    for (const auto& sequence : sequences) {
        auto bad = attach_raw(*service, central);
        control::Bytes combined;
        for (auto value : sequence) {
            control::Frame heartbeat{control::Kind::Heartbeat};
            control::put_u64(heartbeat.payload, value);
            auto bytes = take(control::encode(heartbeat));
            combined.insert(combined.end(), bytes.begin(), bytes.end());
        }
        send_bytes(bad.remote, combined);
        wait_closed(*bad.session);
        const auto failure = bad.session->error();
        check(failure && failure->is(control::ErrorCode::Protocol) &&
                  failure->is_operation(control::ErrorOperation::Heartbeat),
              "invalid heartbeat sequence accepted");
        check_send_cause(*bad.session, *failure);
        bad.session->close();
        check_send_cause(*bad.session, *failure);
        check(bool(healthy_sender->send(application())), "malformed peer stopped another session");
        auto received = take(healthy_receiver->receive_until(control::Clock::now() + operation_timeout));
        check(received.kind == control::Kind::Event && healthy_sender->state() == control::State::Open,
              "healthy peer did not survive protocol rejection");
    }
}

void test_receive_capacity_and_drain() {
    constexpr std::size_t queue_limit = 64;
    constexpr std::size_t byte_limit = 256 * 1024;
    // Small frames reach the count limit; large frames reach the byte limit first.
    for (const auto payload_size : {std::size_t{0}, control::max_payload}) {
        auto service = take(control::Service::create(1));
        auto peer = attach_raw(*service, options());
        const auto accepted_count = (std::min)(queue_limit, byte_limit / (control::header_size + payload_size));
        control::Bytes combined;
        for (std::size_t index = 0; index <= accepted_count; ++index) {
            auto frame = application(payload_size);
            frame.operation = static_cast<std::uint16_t>(index + 1);
            auto bytes = take(control::encode(frame));
            combined.insert(combined.end(), bytes.begin(), bytes.end());
        }
        send_bytes(peer.remote, combined);
        wait_closed(*peer.session);
        const auto failure = peer.session->error();
        check(failure && failure->is(control::ErrorCode::Backpressure) &&
                  failure->is_operation(control::ErrorOperation::Receive),
              "receive overflow did not retain Backpressure");
        // Accepted frames remain readable after closure, in original order.
        for (std::size_t index = 0; index < accepted_count; ++index) {
            auto frame = take(peer.session->receive_until(control::Clock::now()));
            check(frame.operation == index + 1 && frame.payload.size() == payload_size,
                  "closed receive queue lost or reordered accepted frames");
        }
        auto drained = peer.session->try_receive();
        check(!drained && drained.error() == *failure, "drained queue lost the original cause");
        check_send_cause(*peer.session, *failure);
    }
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
    check(!disconnected && !disconnected.error().is(control::ErrorCode::Timeout), "disconnect propagation");
    auto [mismatch_central_socket, mismatch_module_socket] = connected_sockets();
    child.token = "wrong";
    auto wrong = (*service)->attach(std::move(mismatch_central_socket), central);
    auto other = (*service)->attach(std::move(mismatch_module_socket), child);
    check(wrong && other && !(*wrong)->wait_open(control::Clock::now() + operation_timeout),
          "handshake identity mismatch");
    return central;
}

}

void test_hello_policy_version() {
    auto service = take(control::Service::create());
    control::Options options;
    options.token = "policy-version";
    options.generation = 555;
    // Version 0 represents the old payload without the extension. Versions and
    // policy bits are fixed wire values, not timing or capacity configuration.
    for (const auto version : {0u, 1u, control::hello_version}) {
        auto peer = attach_raw(*service, options, false);
        control::Frame hello{control::Kind::Hello};
        control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Module));
        control::put_u64(hello.payload, options.generation);
        check(bool(control::put_string(hello.payload, options.token)), "encode policy token");
        control::put_u32(hello.payload, static_cast<std::uint32_t>(options.heartbeat_interval.count()));
        control::put_u32(hello.payload, static_cast<std::uint32_t>(options.heartbeat_timeout.count()));
        if (version != 0) {
            control::put_u32(hello.payload, version);
            control::put_u32(hello.payload, version == control::hello_version ? 2u : 1u);
        }
        send_frame(peer.remote, hello);
        wait_closed(*peer.session);
        check(peer.session->error()->is(control::ErrorCode::Protocol), "old/version/invalid policy rejected");
    }
}

void test_disabled_expiry() {
    control::Options options;
    options.token = "debug-policy";
    options.generation = 321;
    options.heartbeat_interval = heartbeat_interval;
    options.heartbeat_timeout = heartbeat_timeout;
    options.heartbeat_timeout_enabled = false;
    auto service = take(control::Service::create());
    auto peer = attach_raw(*service, options);
    std::this_thread::sleep_for(heartbeat_observation_time);
    check(peer.session->state() == control::State::Open, "disabled expiry accepts a silent peer");
    send_frame(peer.remote, application());
    check(bool(peer.session->receive_until(control::Clock::now() + operation_timeout)), "resume after silence");
    check(bool(peer.remote.close()), "close raw peer");
    wait_closed(*peer.session);

    auto [server, remote] = connected_sockets();
    auto central = take(service->attach(std::move(server), options));
    options.role = control::Role::Module;
    options.heartbeat_timeout_enabled = true;
    auto child = take(service->attach(std::move(remote), options));
    check(!central->wait_open(control::Clock::now() + operation_timeout), "different expiry policies rejected");
    wait_closed(*child);
}

int main() {
    try {
        control::Frame frame{control::Kind::Request, 42, 7, 19, {std::byte{1}, std::byte{2}}};
        test_framing(frame);
        test_argument_bytes();
        test_disabled_expiry();
        test_hello_policy_version();
        auto central = test_live_sessions(frame);
        test_backpressure_and_timeout(central);
        test_service_lifetime(central);
        test_handshake_timeout_and_normal_close();
        test_bad_heartbeat_isolation();
        test_receive_capacity_and_drain();
        std::cout << "control framing, sessions, heartbeat and disconnect tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
