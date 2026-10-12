#include <vinc/control/channel.hpp>
// Exercise actual Bus code and Socket sessions at allocation failure boundaries.
// Fault checkpoints throw only in this test executable; data-path tests use real mappings.
#include "bus_test_hooks.hpp"
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc_messages/Frame.hpp>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
namespace bus = vinc::message_bus;
namespace detail = vinc::message_bus::detail;
using detail::testing::FaultPoint;
constexpr auto operation_timeout = 5s;
constexpr auto heartbeat_interval = 100ms;
constexpr auto heartbeat_timeout = 15s;
constexpr std::uint64_t message_bytes = 1024;
constexpr std::uint32_t fixture_slots = 2;
constexpr std::uint32_t fixture_readers = 2;
constexpr std::size_t sessions_per_connection = 2;
std::atomic<FaultPoint> armed{FaultPoint::None};
std::atomic<bool> fault_hit{false};
std::atomic<std::size_t> remaining_pending{SIZE_MAX};

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void ok(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

Deadline deadline() {
    return control::Clock::now() + operation_timeout;
}

void arm(FaultPoint point) {
    fault_hit.store(false);
    remaining_pending.store(SIZE_MAX);
    armed.store(point);
}

struct Connection {
    std::unique_ptr<control::Service> service;
    std::shared_ptr<control::Session> central;
    std::shared_ptr<control::Session> module;
    std::shared_ptr<bus::Client> client;
    std::uint64_t next_request = 1;

    Connection() {
        service = take(control::Service::create(sessions_per_connection));
        auto listener = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
        ok(listener.bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0}));
        ok(listener.listen(1));
        const auto address = take(listener.local_address());
        auto outgoing = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
        ok(outgoing.connect_until(address, deadline()));
        auto incoming = take(listener.accept_until(deadline()));
        control::Options options;
        options.role = control::Role::Central;
        options.generation = 1;
        options.token = "bus-cleanup-test";
        options.heartbeat_interval = heartbeat_interval;
        options.heartbeat_timeout = heartbeat_timeout;
        central = take(service->attach(std::move(incoming), options));
        options.role = control::Role::Module;
        module = take(service->attach(std::move(outgoing), options));
        ok(central->wait_open(deadline()));
        ok(module->wait_open(deadline()));
        client = take(detail::RuntimeAccess::create(module));
    }

    ~Connection() {
        detail::RuntimeAccess::shutdown(*client);
    }

    void reply(const control::Frame& request, control::Bytes body) {
        control::Frame response{control::Kind::Response, request.domain, request.operation, request.request};
        control::put_u32(response.payload, detail::result_success);
        response.payload.insert(response.payload.end(), body.begin(), body.end());
        ok(central->send(std::move(response)));
        ok(detail::RuntimeAccess::receive(*client, take(module->receive_until(deadline()))));
    }

    void expect_closed() {
        auto sent = module->send({control::Kind::Event, detail::message_bus_domain, detail::Unsubscribe});
        check(!sent && sent.error().is(control::ErrorCode::Closed), "failed transaction must close session");
        const auto retried = client->subscribe<Frame>("must-not-retry");
        check(!retried && retried.error().is(bus::ErrorCode::Closed), "unknown mutation must reject replay");
    }

    void expect_release(std::uint16_t operation, std::uint64_t id) {
        auto frame = take(central->receive_until(deadline()));
        check(frame.kind == control::Kind::Event && frame.operation == operation && !frame.request,
              "rollback must send release event");
        control::Reader reader(frame.payload);
        check(reader.u64() == id && reader.done(), "rollback must release exactly its registration");
    }
};

detail::Descriptor descriptor() {
    detail::Descriptor result;
    result.id = 41;
    result.owner = 1;
    result.rank = 1;
    result.schema = Frame::schema().hash;
    result.slots = fixture_slots;
    result.readers = fixture_readers;
    result.bytes = message_bytes;
    result.control = detail::shared_name('c');
    result.data = detail::shared_name('d');
    result.counter = detail::shared_name('t');
    result.publisher = "cleanup-fixture";
    return result;
}

control::Bytes advertisement(const detail::Descriptor& value) {
    control::Bytes body;
    ok(detail::encode_descriptor(body, value));
    return body;
}

control::Bytes subscription(std::uint64_t id, const detail::Descriptor* source = nullptr) {
    control::Bytes body;
    control::put_u64(body, id);
    control::put_u32(body, source ? 1 : 0);
    if (source) {
        ok(detail::encode_attachment(body, {*source, 0, 0}));
    }

    return body;
}

void test_send_failure() {
    Connection connection;
    arm(FaultPoint::RequestSend);
    auto result = connection.client->subscribe<Frame>("send-failure");
    check(!result && result.error().is(bus::ErrorCode::ResourceExhausted), "send exception becomes Result");
    check(fault_hit.load() && remaining_pending.load() == 0, "send exception must return pending capacity");
    connection.expect_closed();
}

void test_advertise_failure(FaultPoint point) {
    Connection connection;
    const auto source = descriptor();
    auto pending = std::async(std::launch::async, [&] {
        return connection.client->advertise<Frame>("advertise-failure");
    });
    const auto request = take(connection.central->receive_until(deadline()));
    arm(point);
    connection.reply(request, advertisement(source));
    check(pending.wait_until(deadline()) == std::future_status::ready, "advertise must finish");
    auto result = pending.get();
    check(!result && result.error().is(bus::ErrorCode::ResourceExhausted), "advertise exception becomes Result");
    check(fault_hit.load() && remaining_pending.load() == 0, "advertise failure must return pending capacity");
    if (point == FaultPoint::PublisherAttach) {
        connection.expect_release(detail::ClosePublisher, source.id);
        check(connection.module->state() == control::State::Open, "known rollback need not close session");
    } else {
        connection.expect_closed();
    }
}

void test_subscribe_failure(FaultPoint point) {
    Connection connection;
    const auto source = descriptor();
    constexpr std::uint64_t subscription_id = 42;
    auto pending = std::async(std::launch::async, [&] {
        return connection.client->subscribe<Frame>("subscribe-failure");
    });
    const auto request = take(connection.central->receive_until(deadline()));
    arm(point);
    connection.reply(request, subscription(subscription_id, &source));
    check(pending.wait_until(deadline()) == std::future_status::ready, "subscribe must finish");
    auto result = pending.get();
    check(!result && result.error().is(bus::ErrorCode::ResourceExhausted), "subscribe exception becomes Result");
    check(fault_hit.load() && remaining_pending.load() == 0, "subscribe failure must return pending capacity");
    if (point == FaultPoint::ResponseCopy) {
        connection.expect_closed();
        return;
    }

    connection.expect_release(detail::Unsubscribe, subscription_id);
    check(detail::RuntimeAccess::inspect_attachments(*connection.client).empty(), "rollback removes topology");
    // A known rollback leaves this session usable and releases its local request slot.
    auto next = std::async(std::launch::async, [&] {
        return connection.client->subscribe<Frame>("after-rollback");
    });
    const auto next_request = take(connection.central->receive_until(deadline()));
    connection.reply(next_request, subscription(subscription_id + 1));
    auto subscriber = take(next.get());
    subscriber.close();
    connection.expect_release(detail::Unsubscribe, subscription_id + 1);
}

enum class MalformedReply {
    Truncated,
    Schema,
    ZeroId,
    Dimensions
};

void test_malformed_advertise(MalformedReply kind) {
    Connection connection;
    auto source = descriptor();
    if (kind == MalformedReply::Schema) {
        ++source.schema;
    } else if (kind == MalformedReply::ZeroId) {
        source.id = 0;
    } else if (kind == MalformedReply::Dimensions) {
        source.slots = 0;
    }

    auto body = advertisement(source);
    if (kind == MalformedReply::Truncated) {
        body.pop_back();
    }

    auto pending = std::async(std::launch::async, [&] {
        return connection.client->advertise<Frame>("malformed-success");
    });
    const auto request = take(connection.central->receive_until(deadline()));
    connection.reply(request, std::move(body));
    auto result = pending.get();
    check(!result && result.error().is(bus::ErrorCode::Protocol), "malformed success must report Protocol");
    connection.expect_closed();
}

// Drive the real manager via real control frames, without a module dispatcher.
// These tests own every simulated owner and guarantee no payload access during exited().
Result<control::Bytes> manager_request(detail::BusManager& manager, Connection& connection, std::uint64_t owner,
                               std::uint16_t operation, control::Bytes payload) {
    control::Frame frame{control::Kind::Request, detail::message_bus_domain, operation,
                         connection.next_request++, std::move(payload)};
    ok(connection.module->send(std::move(frame)));
    auto request = take(connection.central->receive_until(deadline()));
    ok(manager.submit(owner, "cleanup-owner", connection.central, std::move(request)));
    auto response = take(connection.module->receive_until(deadline()));
    check(response.kind == control::Kind::Response, "expected manager response");
    auto decoded = take(detail::decode_response(response.payload));
    if (decoded.failure) {
        return std::unexpected(*decoded.failure);
    }

    return control::Bytes(decoded.body.begin(), decoded.body.end());
}

detail::Descriptor register_publisher(detail::BusManager& manager, Connection& connection,
                                      std::uint64_t owner, const std::string& topic) {
    control::Bytes payload;
    ok(control::put_string(payload, topic));
    control::put_u64(payload, Frame::schema().hash);
    control::put_u32(payload, fixture_slots);
    control::put_u64(payload, message_bytes);
    auto body = take(manager_request(manager, connection, owner, detail::Advertise, std::move(payload)));
    control::Reader reader(body);
    auto value = detail::decode_descriptor(reader);
    check(reader.done(), "manager descriptor must be complete");
    return value;
}

detail::Attachment register_subscriber(detail::BusManager& manager, Connection& connection,
                                       std::uint64_t owner, const std::string& topic) {
    control::Bytes payload;
    ok(control::put_string(payload, topic));
    control::put_u64(payload, Frame::schema().hash);
    control::put_u32(payload, 0);
    auto body = take(manager_request(manager, connection, owner, detail::Subscribe, std::move(payload)));
    control::Reader reader(body);
    check(reader.u64() != 0 && reader.u32() == 1, "fixture needs one publisher");
    auto value = detail::decode_attachment(reader);
    check(reader.done(), "manager attachment must be complete");
    return value;
}

template <class Action>
auto client_request(detail::BusManager& manager, Connection& connection, std::uint64_t owner,
                    Action action, FaultPoint point = FaultPoint::None) {
    auto pending = std::async(std::launch::async, std::move(action));
    auto request = take(connection.central->receive_until(deadline()));
    ok(manager.submit(owner, "rollback-owner", connection.central, std::move(request)));
    auto response = take(connection.module->receive_until(deadline()));
    arm(point);
    ok(detail::RuntimeAccess::receive(*connection.client, std::move(response)));
    check(pending.wait_until(deadline()) == std::future_status::ready, "client request must finish");
    return pending.get();
}

void process_release(detail::BusManager& manager, Connection& connection, std::uint64_t owner,
                     std::uint16_t operation) {
    auto event = take(connection.central->receive_until(deadline()));
    check(event.kind == control::Kind::Event && event.operation == operation, "expected rollback event");
    ok(manager.submit(owner, "rollback-owner", connection.central, std::move(event)));
    ok(manager.drain_until(deadline()));
}

void test_topic_discovery() {
    Connection publisher;
    auto manager = take(detail::BusManager::create());
    constexpr std::uint64_t publisher_owner = 201;
    constexpr std::uint64_t reader_owner = 202;
    constexpr std::size_t topic_count = detail::topics_per_page + 3;
    for (std::size_t index = 0; index < topic_count; ++index) {
        (void)register_publisher(*manager, publisher, publisher_owner, "discovery/" + std::to_string(index));
    }
    Connection reader;
    auto first = take(client_request(*manager, reader, reader_owner, [&] { return reader.client->list_topics(); }));
    check(first.topics.size() == detail::topics_per_page && !first.next.empty(), "discovery has bounded pages");
    auto second = take(client_request(*manager, reader, reader_owner, [&] { return reader.client->list_topics(first.next); }));
    check(first.topics.size() + second.topics.size() == topic_count && second.next.empty(), "complete discovery traversal");
    check(second.topics.front().name > first.topics.back().name && second.topics.front().schema == Frame::schema().hash,
        "ordered typed discovery metadata");
    manager->exited(publisher_owner);
    ok(manager->drain_until(deadline()));
    auto empty = take(client_request(*manager, reader, reader_owner, [&] { return reader.client->list_topics(); }));
    check(empty.topics.empty(), "retired topics disappear");
    manager->exited(reader_owner);
    ok(manager->drain_until(deadline()));
}

void test_malformed_discovery() {
    for (int mode = 0; mode < 3; ++mode) {
        Connection connection;
        auto request = std::async(std::launch::async, [&] { return connection.client->list_topics(); });
        const auto frame = take(connection.central->receive_until(deadline()));
        control::Bytes body;
        control::put_u32(body, mode == 0 ? detail::topics_per_page + 1 : 1);
        if (mode != 0) {
            ok(control::put_string(body, "topic"));
            control::put_u64(body, Frame::schema().hash);
            control::put_u32(body, 1);
            if (mode == 2) {
                ok(control::put_string(body, "non-progressing-cursor"));
            }
        }
        connection.reply(frame, std::move(body));
        auto result = request.get();
        check(!result && result.error().is(bus::ErrorCode::Protocol), "reject malformed discovery pages");
    }
}

void test_client_topic_separators() {
    std::size_t sends = 0;
    auto client = take(detail::RuntimeAccess::create(detail::ControlTransport{
        [&](control::Frame) -> Result<void> {
            ++sends;
            // Fail immediately if validation accidentally lets a request escape.
            return std::unexpected(make_error(control::ErrorCode::Closed, control::ErrorOperation::Send));
        },
        [] {}
    }));

    for (const auto* name : {"camera.frames", ".frames", "camera.", "camera/frames.raw"}) {
        auto published = client->advertise<Frame>(name);
        check(!published && published.error().is(bus::ErrorCode::InvalidArgument) &&
                  published.error().is_operation(bus::ErrorOperation::Advertise),
              "client must reject dotted publication names");
        auto subscribed = client->subscribe<Frame>(name);
        check(!subscribed && subscribed.error().is(bus::ErrorCode::InvalidArgument) &&
                  subscribed.error().is_operation(bus::ErrorOperation::Subscribe),
              "client must reject dotted subscription names");
    }

    check(sends == 0, "invalid topic names must not send registration requests");
    detail::RuntimeAccess::shutdown(*client);
}

void test_manager_topic_separators() {
    Connection connection;
    constexpr std::uint64_t owner = 1;
    bus::Limits limits;
    limits.max_topics = 1;
    limits.publishers_per_topic = 1;
    limits.subscribers_per_topic = 1;
    limits.max_shared_bytes = detail::RingBuffer::control_bytes(fixture_slots, 1) +
                              detail::RingBuffer::slot_stride(message_bytes) * fixture_slots +
                              sizeof(detail::SharedWord);
    auto manager = take(detail::BusManager::create(limits));

    // Bypass Client validation with real wire requests. Exact limits reveal any
    // registry, ID or shared-budget mutation left by a rejected request.
    for (const auto operation : {detail::Advertise, detail::Subscribe}) {
        for (const auto* name : {"camera.frames", ".frames", "camera.", "camera/frames.raw"}) {
            control::Bytes payload;
            ok(control::put_string(payload, name));
            control::put_u64(payload, Frame::schema().hash);
            if (operation == detail::Advertise) {
                control::put_u32(payload, fixture_slots);
                control::put_u64(payload, message_bytes);
            } else {
                control::put_u32(payload, 0);
            }

            auto result = manager_request(*manager, connection, owner, operation, std::move(payload));
            const auto expected = operation == detail::Advertise ? bus::ErrorOperation::Advertise
                                                                : bus::ErrorOperation::Subscribe;
            check(!result && result.error().is(bus::ErrorCode::InvalidArgument) &&
                      result.error().is_operation(expected),
                  "manager must reject dotted topic names with the caller operation");
        }
    }

    const auto publisher = register_publisher(*manager, connection, owner, "camera/frames");
    check(publisher.id == 1, "rejected names must not consume publisher IDs");
    const auto subscriber = register_subscriber(*manager, connection, owner, "camera/frames");
    check(subscriber.reader == 0 && subscriber.descriptor.id == publisher.id,
          "rejected names must not consume subscription capacity");
    manager->exited(owner);
    ok(manager->drain_until(deadline()));
}

void test_registration_capacity_reuse() {
    Connection publisher_connection;
    Connection subscriber_connection;
    constexpr std::uint64_t publisher_owner = 1;
    constexpr std::uint64_t subscriber_owner = 2;
    constexpr std::uint32_t one_reader = 1;
    bus::Limits limits;
    limits.max_topics = 1;
    limits.publishers_per_topic = 1;
    limits.subscribers_per_topic = one_reader;
    limits.max_shared_bytes = detail::RingBuffer::control_bytes(fixture_slots, one_reader) +
                              fixture_slots * message_bytes + sizeof(detail::SharedWord);
    auto manager = take(detail::BusManager::create(limits));
    auto advertise = [&] {
        return publisher_connection.client->advertise<Frame>("single-topic", {fixture_slots, message_bytes});
    };
    auto failed_publisher = client_request(*manager, publisher_connection, publisher_owner,
                                           advertise, FaultPoint::CounterOpen);
    check(!failed_publisher && failed_publisher.error().is(bus::ErrorCode::ResourceExhausted) && fault_hit.load(),
          "failure after mapping must roll back publisher");
    process_release(*manager, publisher_connection, publisher_owner, detail::ClosePublisher);
    auto publisher = take(client_request(*manager, publisher_connection, publisher_owner, advertise));

    auto subscribe = [&] {
        return subscriber_connection.client->subscribe<Frame>("single-topic");
    };
    auto failed_subscriber = client_request(*manager, subscriber_connection, subscriber_owner,
                                            subscribe, FaultPoint::AttachmentDecode);
    check(!failed_subscriber && failed_subscriber.error().is(bus::ErrorCode::ResourceExhausted) && fault_hit.load(),
          "failure after topology installation must roll back subscriber");
    process_release(*manager, subscriber_connection, subscriber_owner, detail::Unsubscribe);
    auto subscriber = take(client_request(*manager, subscriber_connection, subscriber_owner, subscribe));
    subscriber.close();
    process_release(*manager, subscriber_connection, subscriber_owner, detail::Unsubscribe);
    publisher.close();
    process_release(*manager, publisher_connection, publisher_owner, detail::ClosePublisher);
    manager->exited(publisher_owner);
    manager->exited(subscriber_owner);
    ok(manager->drain_until(deadline()));
}

void test_exit_cleanup(FaultPoint point) {
    Connection owner;
    Connection failed_observer;
    Connection healthy_observer;
    Connection writer;
    constexpr std::uint64_t owner_id = 1;
    constexpr std::uint64_t failed_observer_id = 2;
    constexpr std::uint64_t healthy_observer_id = 3;
    constexpr std::uint64_t writer_id = 4;
    bus::Limits limits;
    limits.max_topics = 2;
    limits.publishers_per_topic = 1;
    limits.subscribers_per_topic = fixture_readers;
    const auto ring_bytes = detail::RingBuffer::control_bytes(fixture_slots, fixture_readers) +
                            fixture_slots * message_bytes;
    limits.max_shared_bytes = 2 * (ring_bytes + sizeof(detail::SharedWord));
    auto manager = take(detail::BusManager::create(limits));
    const auto first = register_publisher(*manager, owner, owner_id, "a-first");
    auto retired_ring = take(detail::RingBuffer::open(first, false));
    (void)register_subscriber(*manager, failed_observer, failed_observer_id, "a-first");
    (void)register_subscriber(*manager, healthy_observer, healthy_observer_id, "a-first");
    const auto second = register_publisher(*manager, writer, writer_id, "z-last");
    const auto reader = register_subscriber(*manager, owner, owner_id, "z-last");
    auto ring = take(detail::RingBuffer::open(second, true));
    std::atomic<bool> closed{false};
    const auto reservation = take(ring->reserve(closed));
    ok(ring->publish(reservation, detail::minimum_message_bytes, 1, 1));
    auto candidate = ring->candidate(0, false);
    check(candidate && ring->protect(*candidate, reader.reader), "fixture pins reader token");
    const auto next = take(ring->reserve(closed));
    ok(ring->publish(next, detail::minimum_message_bytes, 2, 2));

    // The fixture performs no further work as owner_id. Its pinned token models
    // a process that died at the copy boundary; no concurrent reader exists here.
    arm(point);
    if (point == FaultPoint::None) {
        // Exercise a Result failure from send(), separately from thrown allocation failure.
        failed_observer.central->close();
    }

    manager->exited(owner_id);
    ok(manager->drain_until(deadline()));
    check(point == FaultPoint::None || fault_hit.load(), "Detach failure was injected");
    check(!retired_ring->alive(), "exited publisher must retire");
    check(detail::shared_load(ring->reader(reader.reader).token) == 0,
          "later topic must release dead reader even when earlier Detach fails");
    const auto resumed = take(ring->reserve(closed));
    ring->abandon(resumed);
    const auto notification = take(healthy_observer.module->receive_until(deadline()));
    check(notification.operation == detail::Detach, "other recipients still receive Detach");
    auto failed_send = failed_observer.central->send(
        {control::Kind::Event, detail::message_bus_domain, detail::Detach});
    check(!failed_send, "failed notification closes its recipient");

    // Exact budgets and one Publisher per topic make leaked registry entries or
    // accounting observable. Retained test mappings do not count as active Bus budget.
    const auto replacement = register_publisher(*manager, writer, writer_id, "a-first");
    check(replacement.id != first.id, "publisher capacity and shared budget must be reusable");
    // Drain the expected Attach from the healthy observer before its next request.
    check(take(healthy_observer.module->receive_until(deadline())).operation == detail::Attach,
          "replacement publisher attaches");
    const auto reused = register_subscriber(*manager, healthy_observer, healthy_observer_id, "z-last");
    check(reused.reader == reader.reader, "dead reader index must be reusable");
    manager->exited(failed_observer_id);
    manager->exited(healthy_observer_id);
    manager->exited(writer_id);
    ok(manager->drain_until(deadline()));
}
}

namespace vinc::message_bus::detail::testing {
void checkpoint(FaultPoint point) {
    auto expected = point;
    if (armed.compare_exchange_strong(expected, FaultPoint::None)) {
        fault_hit.store(true);
        throw std::bad_alloc();
    }
}

void pending_remaining(std::size_t count) noexcept {
    remaining_pending.store(count);
}
}

int main() {
    try {
        test_topic_discovery();
        test_malformed_discovery();
        test_client_topic_separators();
        test_manager_topic_separators();
        test_send_failure();
        test_advertise_failure(FaultPoint::ResponseCopy);
        test_advertise_failure(FaultPoint::DescriptorDecode);
        test_advertise_failure(FaultPoint::PublisherAttach);
        test_subscribe_failure(FaultPoint::ResponseCopy);
        test_subscribe_failure(FaultPoint::AttachmentDecode);
        test_malformed_advertise(MalformedReply::Truncated);
        test_malformed_advertise(MalformedReply::Schema);
        test_malformed_advertise(MalformedReply::ZeroId);
        test_malformed_advertise(MalformedReply::Dimensions);
        test_registration_capacity_reuse();
        test_exit_cleanup(FaultPoint::DetachEncode);
        test_exit_cleanup(FaultPoint::DetachSend);
        test_exit_cleanup(FaultPoint::None);
        std::cout << "PASS: Bus transaction rollback and failure-safe exit cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
