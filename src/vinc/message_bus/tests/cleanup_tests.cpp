#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <vinc/message_bus/detail/dispatch.hpp>
#include <vinc/control/channel.hpp>
// Exercise actual Bus code and Socket sessions at allocation failure boundaries.
// Fault checkpoints throw only in this test executable; data-path tests use real mappings.
#include "bus_test_hooks.hpp"
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/topology_protocol.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc/time.hpp>
#include <vinc/common_error.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <algorithm>
#include <atomic>
#include <future>
#include <limits>
#include <set>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#endif

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
// Overrides only this test thread; production has no clock override.
thread_local std::optional<Result<std::uint64_t>> clock_override;
std::atomic<FaultPoint> armed{FaultPoint::None};
std::atomic<bool> fault_hit{false};
std::atomic<bool> reject_page{false};
std::atomic<std::size_t> remaining_pending{SIZE_MAX};
std::atomic<std::uint64_t> failed_notification_owner{0};
std::atomic<std::uint64_t> notification_calls{0};

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

    explicit Connection(bool reject_first_release = false) {
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
        if (reject_first_release) {
            detail::ControlTransport transport;
            transport.send = [session = module, rejected = false](control::Frame frame) mutable -> Result<void> {
                if (!rejected && frame.operation == detail::ReleaseBatch) {
                    rejected = true;
                    return std::unexpected(Error{control::ErrorCode::Backpressure, control::ErrorOperation::Send});
                }

                return session->send(std::move(frame));
            };
            transport.close = [session = module] { session->close(); };
            client = take(detail::RuntimeAccess::create(std::move(transport)));
        } else {
            client = take(detail::RuntimeAccess::create(module));
        }
    }

    ~Connection() {
        detail::RuntimeAccess::shutdown(*client);
    }

    void reply(const control::Frame& request, control::Bytes body, bool malformed = false) {
        control::Frame response{control::Kind::Response, request.domain, request.operation, request.request};
        control::put_u32(response.payload, detail::result_success);
        response.payload.insert(response.payload.end(), body.begin(), body.end());
        ok(central->send(std::move(response)));
        auto received = detail::RuntimeAccess::receive(*client, take(module->receive_until(deadline())));
        if (malformed) {
            check(!received && received.error().is(bus::ErrorCode::Protocol), "dispatcher rejects malformed routes");
        } else {
            ok(std::move(received));
        }
    }

    void expect_closed() {
        auto sent = module->send({control::Kind::Event, detail::message_bus_domain, detail::Unsubscribe});
        check(!sent && sent.error().is(control::ErrorCode::Closed), "failed transaction must close session");
        const auto retried = client->subscribe<Frame>("must-not-retry");
        check(!retried && retried.error().is(bus::ErrorCode::Closed), "unknown mutation must reject replay");
    }

    void expect_release(std::uint16_t operation, std::uint64_t id) {
        ok(detail::RuntimeAccess::progress_control(*client));
        auto frame = take(central->receive_until(deadline()));
        check(frame.kind == control::Kind::Request && frame.operation == detail::ReleaseBatch && frame.request,
              "release requires a correlated acknowledgement");
        control::Reader reader(frame.payload);
        check(reader.u32() == 1 && reader.u32() == operation && reader.u64() == id && reader.done(),
              "rollback must release exactly its registration");
        reply(frame, {});
    }
};

// Inspect the worker's real page while standing in for the dispatcher.
control::Frame receive_single_change(detail::BusManager& manager, Connection& connection, std::uint64_t owner) {
    auto frame = take(connection.module->receive_until(deadline()));
    check(frame.operation == detail::SyncPage, "expected a topology transfer");
    auto page = take(detail::decode_topology_page(frame.payload));
    check(page.final && page.mode == detail::TopologyMode::Delta, "single delta page");
    control::Frame change;
    ok(detail::visit_topology_records(page.records, [&](auto kind, auto body) -> Result<void> {
        check(!change.operation, "one mutation in this transfer");
        change.operation = static_cast<std::uint16_t>(kind);
        change.payload.assign(body.begin(), body.end());
        return {};
    }));
    auto ack = take(detail::encode_topology_ack({page.id, detail::TopologyReceipt::Applied}));
    ok(manager.submit(owner, "fixture", connection.central,
        {control::Kind::Event, detail::message_bus_domain, detail::SyncAck, 0, std::move(ack)}));
    return change;
}

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
    control::put_u32(body, fixture_slots);
    ok(detail::encode_descriptor(body, value));
    control::put_u32(body, 0); // No callback destinations in this synthetic reply.
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

void test_notification_install_catchup() {
    Connection connection;
    const auto source = descriptor();
    auto ring = take(detail::RingBuffer::create(source));
    auto counter = take(detail::NamedCounter::create(source.counter));
    const auto name = detail::shared_name('n');
    auto receiver = take(osal::NotificationReceiver::create(name));
    struct Cleanup {
        const std::string& name;
        ~Cleanup() {
            (void)osal::NotificationReceiver::unlink_name(name);
        }
    } cleanup{name};
    auto pending = std::async(std::launch::async, [&] {
        return connection.client->advertise<Frame>("snapshot-catchup");
    });
    connection.reply(take(connection.central->receive_until(deadline())), advertisement(source));
    auto publisher = take(pending.get());
    auto local = make_message<Frame>();
    local->data().sequence = 42;
    ok(publisher.push(*local));
    check(detail::shared_load(ring->header().head) == 1, "publication precedes route installation");

    std::uint64_t revision = 0;
    auto install = [&](bool enabled) {
        control::Frame frame{control::Kind::Event, detail::message_bus_domain, detail::NotificationRoutesChanged};
        control::put_u64(frame.payload, source.id);
        control::put_u32(frame.payload, enabled ? 1 : 0);
        if (enabled) {
            control::put_u64(frame.payload, 2);
            ok(control::put_string(frame.payload, name));
        }
        control::Bytes record;
        detail::append_topology_record(record, frame.operation, frame.payload);
        ++revision;
        frame.operation = detail::SyncPage;
        frame.payload = take(detail::encode_topology_page({{1, revision, 0}, revision - 1, revision,
            detail::TopologyMode::Delta, true, record}));
        ok(detail::RuntimeAccess::receive(*connection.client, std::move(frame)));
        check(take(connection.central->receive_until(deadline())).operation == detail::SyncAck, "receipt");
    };
    install(true);
    ok(receiver.wait_until(deadline()));
    // Reusing a healthy sender must still catch up without a later publication.
    install(true);
    ok(receiver.wait_until(deadline()));
    install(false);
    ok(publisher.push(*local));
    auto empty = receiver.wait_until(std::chrono::steady_clock::now());
    check(!empty && empty.error().is(osal::ErrorCode::Timeout), "removed route must receive no new notification");
    // Fixture teardown owns the remaining registration.
}

void test_publication_notification_failure() {
    Connection connection;
    const auto source = descriptor();
    auto ring = take(detail::RingBuffer::create(source));
    auto counter = take(detail::NamedCounter::create(source.counter));
    const auto first_name = detail::shared_name('n');
    const auto second_name = detail::shared_name('n');
    auto first = take(osal::NotificationReceiver::create(first_name));
    auto second = take(osal::NotificationReceiver::create(second_name));
    struct Cleanup {
        std::string first;
        std::string second;
        ~Cleanup() {
            (void)osal::NotificationReceiver::unlink_name(first);
            (void)osal::NotificationReceiver::unlink_name(second);
        }
    } cleanup{first_name, second_name};
    auto pending = std::async(std::launch::async, [&] {
        return connection.client->advertise<Frame>("notification-test");
    });
    control::Bytes body;
    control::put_u32(body, fixture_slots);
    ok(detail::encode_descriptor(body, source));
    control::put_u32(body, 2);
    control::put_u64(body, 2);
    ok(control::put_string(body, first_name));
    control::put_u64(body, 3);
    ok(control::put_string(body, second_name));
    connection.reply(take(connection.central->receive_until(deadline())), std::move(body));
    auto publisher = take(pending.get());
    ok(first.wait_until(deadline()));
    ok(second.wait_until(deadline()));
    auto local = make_message<Frame>();
    local->data().sequence = 42;
    failed_notification_owner.store(2);
    notification_calls.store(0);
    ok(publisher.push(*local));
    ok(second.wait_until(deadline()));
    check(detail::shared_load(ring->header().head) == 1, "signal follows committed head");
    auto status = detail::RuntimeAccess::notification_diagnostics(*connection.client);
    check(status.degraded && status.target == 2 && status.failures == 1,
        "first notify failure preserves publication and target diagnostics");
    auto loan = take(publisher.get_next());
    loan->data().sequence = 43;
    ok(publisher.commit(loan));
    ok(second.wait_until(deadline()));
    check(detail::shared_load(ring->header().head) == 2, "loan publication remains successful");
    check(notification_calls.load() == 4, "first failure permits the next normal publication retry");
    ok(publisher.push(*local));
    ok(second.wait_until(deadline()));
    check(notification_calls.load() == 5, "repeated sender failure is rate limited, other routes progress");
    failed_notification_owner.store(0);
    std::this_thread::sleep_for(config::notification_retry_interval);
    detail::RuntimeAccess::progress_notifications(*connection.client);
    ok(first.wait_until(deadline()));
    status = detail::RuntimeAccess::notification_diagnostics(*connection.client);
    check(!status.degraded && status.recoveries == 1 && status.last_error.has_value(),
        "low-frequency reopen catches up and retains failure history");
    ok(publisher.push(*local));
    ok(first.wait_until(deadline()));
    control::Frame invalid{control::Kind::Event, detail::message_bus_domain, detail::NotificationRoutesChanged};
    control::put_u64(invalid.payload, 0);
    control::put_u32(invalid.payload, 0);
    auto rejected = detail::RuntimeAccess::receive(*connection.client, std::move(invalid));
    check(!rejected && rejected.error().is(bus::ErrorCode::Protocol), "zero Publisher route identity is malformed");
}

void test_image_validation() {
    namespace format = message::detail::format;
    Connection connection;
    auto source = descriptor();
    // The fixture writes wire bytes directly, so malformed input reaches the real reader.
    auto ring = take(detail::RingBuffer::create(source));
    auto pending = std::async(std::launch::async, [&] {
        return detail::ImageAccess::subscribe(*connection.client, "image-validation", source.schema);
    });
    connection.reply(take(connection.central->receive_until(deadline())), subscription(42, &source));
    auto images = take(pending.get());
    auto typed_pending = std::async(std::launch::async, [&] {
        return connection.client->subscribe<Frame>("image-validation");
    });
    control::Bytes typed_body;
    control::put_u64(typed_body, 43);
    control::put_u32(typed_body, 1);
    ok(detail::encode_attachment(typed_body, {source, 1, 0}));
    connection.reply(take(connection.central->receive_until(deadline())), std::move(typed_body));
    auto typed = take(typed_pending.get());
    auto message = make_message<Frame>();
    message->data().sequence = 17;
    const auto original = take(message->snapshot());
    auto bytes = std::vector<std::byte>(original.bytes().begin(), original.bytes().end());
    auto read = [&](std::size_t offset) {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < format::integer_bytes; ++index) {
            value |= std::uint64_t(std::to_integer<unsigned>(bytes.at(offset + index))) << (index * 8);
        }
        return value;
    };
    auto write = [&](std::size_t offset, std::uint64_t value, unsigned width = format::integer_bytes) {
        for (unsigned index = 0; index < width; ++index) {
            bytes.at(offset + index) = std::byte((value >> (index * 8)) & 255);
        }
    };
    const auto table = read(format::header::descriptor_table);
    const auto root = read(format::header::size);
    const auto root_descriptor = table + root * format::descriptor::size;
    const auto first_field = read(read(root_descriptor));
    const auto field_descriptor = table + first_field * format::descriptor::size;
    std::atomic<bool> closed{false};
    std::uint64_t count = 0;
    auto publish_bytes = [&] {
        auto reserved = take(ring->reserve(closed));
        check(bytes.size() <= reserved.bytes.size(), "image fixture capacity");
        std::copy(bytes.begin(), bytes.end(), reserved.bytes.begin());
        ++count;
        ok(ring->publish(reserved, bytes.size(), count, count, count));
    };
    publish_bytes();
    auto valid = take(images.get_last());
    auto typed_value = take(typed.get_last());
    check(valid->publisher_id == source.id && valid->publisher_name == source.publisher &&
          valid->timestamp == typed_value->get_message_timestamp() &&
          valid->monotonic_timestamp == typed_value->get_message_monotonic_timestamp() &&
          valid->topic_count == typed_value->get_topic_count(), "typed/image metadata mismatch");

    // uint64 -> double has the same layout width, but disagrees with Frame's field schema.
    constexpr std::uint32_t double_scalar_code = 10;
    write(field_descriptor + format::descriptor::scalar, double_scalar_code, format::tag_bytes);
    publish_bytes();
    auto structural = take(images.get_last());
    auto rejected = typed.get_last();
    check(!rejected && rejected.error().is(bus::ErrorCode::TypeMismatch), "typed field validation relaxed");
    auto canonical = take(message::detail::FrameworkAccess::canonical_image(structural->image, message_bytes));
    check(!Frame::from_bytes(canonical.bytes()), "generic validation claimed typed compatibility");
    check(detail::shared_load(ring->reader(0).token) == 0 && detail::shared_load(ring->reader(1).token) == 0,
          "validation retained a shared Reader lease");

    write(root_descriptor + format::descriptor::schema_hash, source.schema ^ 1);
    publish_bytes();
    auto batch = take(detail::capture(detail::ImageAccess::core(images)));
    // Consume the retained structural record, then repeatedly reject the bad one.
    take(detail::read_image_batch(detail::ImageAccess::core(images), batch));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        auto failed = detail::read_image_batch(detail::ImageAccess::core(images), batch);
        check(!failed && failed.error().is(bus::ErrorCode::TypeMismatch), "failed image advanced cursor");
    }
    auto bad = images.get_last();
    check(!bad && bad.error().is(bus::ErrorCode::TypeMismatch) && !bad.error().causes().empty(),
          "image schema mismatch lost storage cause");
    write(root_descriptor + format::descriptor::schema_hash, source.schema);
    write(format::header::descriptor_table, UINT64_MAX);
    publish_bytes();
    bad = images.get_last();
    check(!bad && bad.error().is(bus::ErrorCode::InvalidData), "malformed image accepted");
    check(detail::shared_load(ring->reader(0).token) == 0, "failed image validation retained lease");
    check(take(Frame::from_bytes(valid->image.bytes()))->data().sequence.get() == 17,
          "later corruption changed retained image");

    // An unknown hash needs no C++ class or decoder registry.
    bytes.assign(original.bytes().begin(), original.bytes().end());
    constexpr std::uint64_t unknown_hash = 0x123456789abcdefULL;
    write(root_descriptor + format::descriptor::schema_hash, unknown_hash);
    source = descriptor();
    source.schema = unknown_hash;
    auto unknown_ring = take(detail::RingBuffer::create(source));
    auto unknown_pending = std::async(std::launch::async, [&] {
        return detail::ImageAccess::subscribe(*connection.client, "unknown-image", unknown_hash);
    });
    connection.reply(take(connection.central->receive_until(deadline())), subscription(44, &source));
    auto unknown = take(unknown_pending.get());
    auto reserved = take(unknown_ring->reserve(closed));
    std::copy(bytes.begin(), bytes.end(), reserved.bytes.begin());
    ok(unknown_ring->publish(reserved, bytes.size(), 1, 1, 1));
    check(take(unknown.get_last())->schema_hash == unknown_hash, "unknown schema was not delivered");
}

void test_publication_times() {
    Connection connection;
    const auto source = descriptor();
    auto ring = take(detail::RingBuffer::create(source));
    auto counter = take(detail::NamedCounter::create(source.counter));
    auto advertise = std::async(std::launch::async, [&] {
        return connection.client->advertise<Frame>("time-test");
    });
    connection.reply(take(connection.central->receive_until(deadline())), advertisement(source));
    auto publisher = take(advertise.get());
    auto subscribe = std::async(std::launch::async, [&] {
        return connection.client->subscribe<Frame>("time-test");
    });
    connection.reply(take(connection.central->receive_until(deadline())), subscription(42, &source));
    auto subscriber = take(subscribe.get());

    auto local = make_message<Frame>();
    local->data().sequence = 42;
    clock_override = 1000;
    ok(publisher.push(*local));
    auto first = take(subscriber.get_last());
    check(first->get_message_timestamp() == 1000 && first->get_message_monotonic_timestamp() > 0,
          "push delivers both framework clocks");
    check(local->get_message_timestamp() == 0 && local->get_message_monotonic_timestamp() == 0,
          "push never mutates source metadata");
    auto copy = std::make_unique<Frame>(*first);
    check(copy->get_message_timestamp() == first->get_message_timestamp() &&
              copy->get_message_monotonic_timestamp() == first->get_message_monotonic_timestamp(),
          "message copy preserves both publication clocks");
    auto serialized = std::vector<std::uint8_t>{};
    ok(first->serialize(serialized));
    ok(copy->deserialize(serialized));
    check(copy->get_message_timestamp() == first->get_message_timestamp() &&
              copy->get_message_monotonic_timestamp() == first->get_message_monotonic_timestamp(),
          "payload decoding preserves both destination timestamps");

    clock_override = 500;
    ok(publisher.push(*first));
    auto replayed = take(subscriber.get_last());
    check(replayed->get_message_timestamp() == 500 &&
              replayed->get_message_monotonic_timestamp() >= first->get_message_monotonic_timestamp(),
          "republishing replaces source times even when UTC moves backward");
    check(first->get_message_timestamp() == 1000, "republishing preserves the local received message");
    auto loan = take(publisher.get_next());
    clock_override = 0;
    ok(publisher.commit(loan));
    auto committed = take(subscriber.get_last());
    check(committed->get_message_timestamp() == 0 && committed->get_message_monotonic_timestamp() > 0,
          "commit samples at publication, and Unix epoch zero is valid");

    const auto published = detail::shared_load(ring->header().head);
    clock_override = std::unexpected(Error{common::ErrorCode::TimestampOutOfRange, common::ErrorOperation::ReadTime});
    auto failed = publisher.push(*local);
    check(!failed && failed.error().is(bus::ErrorCode::TimestampUnavailable) &&
              failed.error().is_operation(bus::ErrorOperation::Push) &&
              failed.error().causes().size() == 1 &&
              failed.error().causes().front().domain == ErrorDomain::Common,
          "clock failure returns Bus classification and preserves Common cause");
    check(detail::shared_load(ring->header().head) == published, "failed push cannot publish partial metadata");
    clock_override = (std::numeric_limits<std::uint64_t>::max)();
    ok(publisher.push(*local));
    auto maximum = take(subscriber.get_last());
    check(maximum->get_message_timestamp() == (std::numeric_limits<std::uint64_t>::max)(),
          "retry after failed push preserves all timestamp bits");
    auto failed_loan = take(publisher.get_next());
    const auto before_failure = detail::shared_load(ring->header().head);
    clock_override = std::unexpected(Error{common::ErrorCode::TimestampOutOfRange, common::ErrorOperation::ReadTime});
    failed = publisher.commit(failed_loan);
    check(!failed && failed.error().is(bus::ErrorCode::TimestampUnavailable) &&
              failed.error().is_operation(bus::ErrorOperation::Commit), "commit reports clock failure");
    check(detail::shared_load(ring->header().head) == before_failure, "failed commit leaves publication head unchanged");
    auto outstanding = publisher.get_next();
    check(!outstanding && outstanding.error().is(bus::ErrorCode::OutstandingLoan),
          "post-seal failure retains existing outstanding-loan policy");
    clock_override.reset();
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
    auto closing = std::async(std::launch::async, [&] { return subscriber.close(); });
    connection.expect_release(detail::Unsubscribe, subscription_id + 1);
    ok(closing.get());
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
    connection.reply(request, std::move(body),
        kind == MalformedReply::Truncated || kind == MalformedReply::ZeroId);
    auto result = pending.get();
    check(!result && result.error().is(bus::ErrorCode::Protocol), "malformed success must report Protocol");
    connection.expect_closed();
}

// Drive the real manager via real control frames, without a module dispatcher.
// These tests own every simulated owner and guarantee no payload access during exited().
Result<control::Bytes> manager_request(detail::BusManager& manager, Connection& connection, std::uint64_t owner,
                               std::uint16_t operation, control::Bytes payload, std::string notification = {}) {
    control::Frame frame{control::Kind::Request, detail::message_bus_domain, operation,
                         connection.next_request++, std::move(payload)};
    ok(connection.module->send(std::move(frame)));
    auto request = take(connection.central->receive_until(deadline()));
    ok(manager.submit(owner, "cleanup-owner", connection.central, std::move(request), std::move(notification)));
    auto response = take(connection.module->receive_until(deadline()));
    check(response.kind == control::Kind::Response, "expected manager response");
    auto decoded = take(detail::decode_response(response.payload));
    if (decoded.failure) {
        return std::unexpected(*decoded.failure);
    }

    return control::Bytes(decoded.body.begin(), decoded.body.end());
}

enum class CounterFault { None, Reserve, Retire, RetireAllocation, Size, SizeAndUnlink, Map };
#ifdef __linux__
thread_local CounterFault native_counter_fault = CounterFault::None;
// Native calls made on the fixture thread only. Names identify the exact
// failed allocation even when the other half of the ring also needs cleanup.
thread_local unsigned ring_maps_before_failure = 0;
thread_local unsigned ring_sizes_before_failure = 0;
thread_local std::string_view ring_unlink_denied;
#endif

// One fixture represents one journal reservation. The Bus worker is its only
// writer. Tests inspect it after drain_until synchronizes with that worker.
struct CounterJournal final : vinc::detail::ResourceNames {
    explicit CounterJournal(CounterFault fault) : fault(fault) {}
    CounterFault fault;
    std::string name = detail::shared_name('t');
    bool reserved = false;
    unsigned retire_calls = 0;
    const Error journal_error{osal::ErrorCode::PermissionDenied};

    Result<std::string> reserve() override {
        if (fault == CounterFault::Reserve) {
            return std::unexpected(journal_error);
        }

        reserved = true;
#ifdef __linux__
        native_counter_fault = fault;
#endif
        return name;
    }

    Result<void> retire(std::string_view requested) override {
        check(reserved && requested == name, "retired unowned reservation");
        ++retire_calls;
        if (fault == CounterFault::RetireAllocation) {
            throw std::bad_alloc();
        }

        if (fault == CounterFault::Retire) {
            return std::unexpected(journal_error);
        }

        reserved = false;
        return {};
    }
};

void test_counter_registration(CounterFault fault, bool collide = false) {
    auto journal = std::make_shared<CounterJournal>(fault);
    // An independent owner preserves its mapping and initial value across collision.
    std::shared_ptr<detail::NamedCounter> original;
    if (collide) {
        original = take(detail::NamedCounter::create(journal->name));
        check(take(original->next()) == 1, "initialize original counter");
    }

    Connection connection;
    auto manager = take(detail::BusManager::create({}, journal));
    control::Bytes payload;
    ok(control::put_string(payload, "registration/test"));
    control::put_u64(payload, Frame::schema().hash);
    control::put_u32(payload, 0);
    constexpr std::uint64_t owner = 1;
    auto result = manager_request(*manager, connection, owner, detail::Subscribe, std::move(payload));
    ok(manager->drain_until(deadline()));
    auto diagnostic = manager->last_registration_cleanup_failure();

    if (fault == CounterFault::Reserve) {
        check(!result && result.error().is_operation(bus::ErrorOperation::CreateTopic), "reserve failure");
        check(!journal->reserved && journal->retire_calls == 0 && !diagnostic, "retired failed reserve");
        return;
    }

    if (fault == CounterFault::None && !collide) {
        check(result && journal->reserved && journal->retire_calls == 0, "successful counter handoff");
        manager->exited(owner);
        ok(manager->drain_until(deadline()));
        check(!journal->reserved && journal->retire_calls == 1 && !diagnostic, "counter owner retirement");
        return;
    }

    check(!result && result.error().is(bus::ErrorCode::Io), "unexpected counter failure");
    const auto operation = fault == CounterFault::Map ? bus::ErrorOperation::MapCounter
                                                       : bus::ErrorOperation::CreateCounter;
    check(result.error().is_operation(operation), "lost primary operation");
    const auto causes = result.error().causes();
    check(!causes.empty() && causes.back().domain == ErrorDomain::Osal, "lost OSAL primary failure");
    if (collide) {
        check(causes.back().code == static_cast<int>(osal::ErrorCode::AlreadyExists), "lost collision cause");
        auto reopened = take(detail::NamedCounter::open(journal->name));
        check(take(reopened->next()) == 2, "colliding object changed or lost");
    }

#ifdef __linux__
    if (fault == CounterFault::Map) {
        check(causes.back().native.value() == ENOMEM, "lost native mapping failure");
    }
    if (fault == CounterFault::Size || fault == CounterFault::SizeAndUnlink) {
        check(causes.back().native.value() == ENOSPC, "lost native initialization failure");
    }
#endif
    if (fault == CounterFault::Retire || fault == CounterFault::RetireAllocation || fault == CounterFault::SizeAndUnlink) {
        check(journal->reserved && diagnostic && diagnostic->counter && diagnostic->counter->resource_name == journal->name &&
              diagnostic->primary == result.error(), "lost recovery evidence or primary cause");
        if (fault == CounterFault::Retire) {
            check(journal->retire_calls == 1 && diagnostic->counter->cleanup == journal->journal_error,
                  "journal removal failure lost or retried");
        } else if (fault == CounterFault::RetireAllocation) {
            check(journal->retire_calls == 1 && diagnostic->counter->cleanup.is(bus::ErrorCode::ResourceExhausted),
                  "allocation during retirement hid the primary failure");
        } else {
#ifdef __linux__
            check(journal->retire_calls == 0 && diagnostic->counter->cleanup.is_operation(osal::ErrorOperation::UnlinkSharedMemory) &&
                  diagnostic->counter->cleanup.native.value() == EACCES && causes.back().native.value() == ENOSPC,
                  "uncertain unlink retired reservation or replaced primary errno");
            auto retained = take(osal::SharedMemory::open(journal->name, osal::SharedMemoryAccess::ReadOnly));
            ok(osal::SharedMemory::unlink_name(journal->name));
#endif
        }
        return;
    }

    check(!diagnostic && !journal->reserved && journal->retire_calls == 1, "confirmed rollback must retire once");
    if (!collide) {
        auto missing = osal::SharedMemory::open(journal->name, osal::SharedMemoryAccess::ReadOnly);
        check(!missing && missing.error().is(osal::ErrorCode::NotFound), "rollback left an owned mapping");
    }
}

control::Bytes advertise_payload(const std::string& topic) {
    control::Bytes payload;
    ok(control::put_string(payload, topic));
    control::put_u64(payload, Frame::schema().hash);
    control::put_u32(payload, detail::publisher_options_mask);
    control::put_u32(payload, fixture_slots);
    control::put_u64(payload, message_bytes);
    return payload;
}

detail::Descriptor register_publisher(detail::BusManager& manager, Connection& connection,
                                      std::uint64_t owner, const std::string& topic, std::uint32_t expected_routes = 0) {
    auto body = take(manager_request(manager, connection, owner, detail::Advertise, advertise_payload(topic)));
    control::Reader reader(body);
    check(reader.u32() == fixture_slots, "Advertise resolved request count");
    auto value = detail::decode_descriptor(reader);
    const auto routes = reader.u32();
    check(routes == expected_routes, "Advertise route count");
    for (std::uint32_t index = 0; index < routes; ++index) {
        check(reader.u64() != 0 && !reader.string().empty(), "authenticated destination identity");
    }
    check(reader.done(), "manager descriptor and routes must be complete");
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
                    Action action, FaultPoint point = FaultPoint::None, std::string notification = {}) {
    auto pending = std::async(std::launch::async, std::move(action));
    auto request = take(connection.central->receive_until(deadline()));
    ok(manager.submit(owner, "rollback-owner", connection.central, std::move(request), std::move(notification)));
    auto response = take(connection.module->receive_until(deadline()));
    arm(point);
    ok(detail::RuntimeAccess::receive(*connection.client, std::move(response)));
    check(pending.wait_until(deadline()) == std::future_status::ready, "client request must finish");
    return pending.get();
}

void test_image_notification_route() {
    Connection writer;
    Connection reader;
    auto manager = take(detail::BusManager::create());
    const auto name = detail::shared_name('n');
    auto receiver = take(osal::NotificationReceiver::create(name));
    struct Cleanup {
        const std::string& name;
        ~Cleanup() {
            (void)osal::NotificationReceiver::unlink_name(name);
        }
    } cleanup{name};
    const std::string topic = "images/notification";
    auto publisher = take(client_request(*manager, writer, 1, [&] {
        return writer.client->advertise<Frame>(topic, {.slots = fixture_slots, .max_message_bytes = message_bytes});
    }));
    auto subscriber = take(client_request(*manager, reader, 2, [&] {
        return detail::ImageAccess::subscribe(*reader.client, topic, Frame::schema().hash);
    }, FaultPoint::None, name));
    ok(client_request(*manager, reader, 2, [&] {
        return detail::enable_notifications(detail::ImageAccess::core(subscriber));
    }, FaultPoint::None, name));
    // Install the real central route and its receipt through the Host dispatcher.
    ok(detail::RuntimeAccess::receive(*writer.client, take(writer.module->receive_until(deadline()))));
    ok(manager->submit(1, "rollback-owner", writer.central, take(writer.central->receive_until(deadline()))));
    ok(receiver.wait_until(deadline())); // Route installation catch-up hint.
    auto empty = receiver.wait_until(control::Clock::now());
    check(!empty && empty.error().is(osal::ErrorCode::Timeout), "notification was not drained");
    auto value = make_message<Frame>();
    value->data().sequence = 51;
    ok(publisher.push(*value));
    // This is a native notification wait, not Context's periodic fallback.
    ok(receiver.wait_until(deadline()));
    auto batch = take(detail::capture(detail::ImageAccess::core(subscriber)));
    auto image = take(detail::read_image_batch(detail::ImageAccess::core(subscriber), batch));
    check(take(Frame::from_bytes(image->image.bytes()))->data().sequence.get() == 51,
          "notified image payload was not dispatchable");
}

void process_release(detail::BusManager& manager, Connection& connection, std::uint64_t owner,
                     std::uint16_t operation) {
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto event = take(connection.central->receive_until(deadline()));
    check(event.kind == control::Kind::Request && event.operation == detail::ReleaseBatch, "expected release batch");
    control::Reader reader(event.payload);
    check(reader.u32() == 1 && reader.u32() == operation, "expected single release kind");
    ok(manager.submit(owner, "rollback-owner", connection.central, std::move(event)));
    ok(manager.drain_until(deadline()));
    ok(detail::RuntimeAccess::receive(*connection.client, take(connection.module->receive_until(deadline()))));
}

void test_topic_limit_read_failure() {
    Connection connection;
    constexpr std::uint64_t owner = 33;
    const Error read_error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory};
    std::atomic<bool> unavailable{false};
    auto manager = take(detail::BusManager::create({}, {}, {}, [&]() -> Result<std::uint32_t> {
        if (unavailable.load()) {
            return std::unexpected(read_error);
        }

        return 0;
    }));
    auto subscribe = [&](std::string name) {
        return client_request(*manager, connection, owner, [&] {
            return connection.client->subscribe<Frame>(std::move(name));
        });
    };
    auto original = take(subscribe("limit/original"));
    unavailable.store(true);
    auto same = take(subscribe("limit/original"));
    auto rejected = subscribe("limit/new");
    check(!rejected && rejected.error().is(bus::ErrorCode::Io) &&
          rejected.error().is_operation(bus::ErrorOperation::CreateTopic) &&
          rejected.error().causes().size() == 1 && rejected.error().causes().front() == read_error.record(),
          "a failed limit lookup rejects the new name and preserves the cause");
    unavailable.store(false);
    auto retry = take(subscribe("limit/new"));
}

void test_publisher_defaults() {
    Connection connection;
    constexpr std::uint64_t owner = 31;
    constexpr std::uint32_t initial_slots = 3;
    constexpr std::uint32_t changed_slots = 4;
    constexpr std::uint64_t changed_bytes = 2048;
    std::atomic<std::uint32_t> slots{initial_slots};
    std::atomic<std::uint64_t> bytes{message_bytes};
    std::atomic<std::size_t> reads{0};
    std::atomic<bool> failed{false};
    auto manager = take(detail::BusManager::create({}, {}, [&]() -> Result<detail::PublisherDefaults> {
        ++reads;
        if (failed.load()) {
            return std::unexpected(Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory});
        }

        return detail::PublisherDefaults{slots.load(), bytes.load()};
    }));
    const auto advertise = [&](std::string name, bus::PublisherOptions options = {}) {
        return client_request(*manager, connection, owner, [&] {
            return connection.client->advertise<Frame>(std::move(name), options);
        });
    };
    auto original = take(advertise("defaults/original"));
    check(original.stats().requested_slots == initial_slots && original.stats().max_message_bytes == message_bytes,
          "omitted options resolve at registration");

    slots.store(changed_slots);
    bytes.store(changed_bytes);
    auto bytes_only = take(advertise("defaults/bytes", {.max_message_bytes = message_bytes}));
    auto slots_only = take(advertise("defaults/slots", {.slots = initial_slots}));
    check(bytes_only.stats().effective_slots == changed_slots && bytes_only.stats().max_message_bytes == message_bytes,
          "explicit bytes retain dynamic default slots");
    check(slots_only.stats().effective_slots == initial_slots && slots_only.stats().max_message_bytes == changed_bytes,
          "explicit slots retain dynamic default bytes");
    check(original.stats().effective_slots == initial_slots && original.stats().max_message_bytes == message_bytes,
          "changing defaults never resizes an existing ring");

    const auto reads_before_explicit = reads.load();
    failed.store(true);
    auto explicit_options = take(advertise("defaults/explicit", {.slots = 1, .max_message_bytes = message_bytes}));
    check(explicit_options.stats().requested_slots == 1 && explicit_options.stats().effective_slots == 2,
          "explicit one-slot request keeps its original statistics and two-slot minimum");
    auto local = make_message<Frame>();
    ok(explicit_options.push(*local));
    check(reads.load() == reads_before_explicit, "explicit options and publications never read Param");

    const auto invalid_slots = advertise("defaults/zero-slots", {.slots = 0});
    const auto invalid_bytes = advertise("defaults/zero-bytes", {.max_message_bytes = 0});
    const auto oversized_bytes = advertise("defaults/oversized-bytes",
        {.max_message_bytes = config::message_bus_default_shared_bytes + 1});
    check(!invalid_slots && invalid_slots.error().is(bus::ErrorCode::InvalidArgument) &&
          !invalid_bytes && invalid_bytes.error().is(bus::ErrorCode::InvalidArgument) &&
          !oversized_bytes && oversized_bytes.error().is(bus::ErrorCode::InvalidArgument),
          "invalid explicit dimensions never resolve defaults");
    check(reads.load() == reads_before_explicit, "invalid explicit fields fail before consulting defaults");

    const auto unavailable = advertise("defaults/unavailable");
    check(!unavailable && unavailable.error().is(bus::ErrorCode::Io) &&
          unavailable.error().is_operation(bus::ErrorOperation::Advertise) &&
          unavailable.error().causes().size() == 1 &&
          unavailable.error().causes().front().domain == ErrorDomain::Osal,
          "default-source failure retains the public Bus operation and its cause");
    failed.store(false);
    auto retried = take(advertise("defaults/unavailable"));
    check(retried.stats().effective_slots == changed_slots, "a failed defaults read can be retried");

    // Raw requests have their own monotonically increasing request identity;
    // never reuse Client's authenticated owner with an independent counter.
    Connection malformed_connection;
    constexpr std::uint64_t malformed_owner = owner + 1;
    auto malformed = [&](control::Bytes payload) {
        const auto before = reads.load();
        auto result = manager_request(*manager, malformed_connection, malformed_owner,
            detail::Advertise, std::move(payload));
        check(!result && result.error().is(bus::ErrorCode::InvalidArgument), "malformed options rejected");
        check(reads.load() == before, "malformed input must not read or repair defaults");
    };
    control::Bytes prefix;
    ok(control::put_string(prefix, "defaults/malformed"));
    control::put_u64(prefix, Frame::schema().hash);
    malformed(prefix);
    auto unknown = prefix;
    constexpr auto unknown_flag = detail::publisher_options_mask + 1;
    control::put_u32(unknown, unknown_flag);
    malformed(std::move(unknown));
    auto truncated = prefix;
    control::put_u32(truncated, detail::publisher_options_mask);
    control::put_u32(truncated, fixture_slots);
    malformed(std::move(truncated));
    auto trailing = prefix;
    control::put_u32(trailing, 0);
    control::put_u32(trailing, fixture_slots);
    malformed(std::move(trailing));
    for (const auto old_operation : {std::uint16_t{0x0201}, std::uint16_t{0x0301}}) {
        const auto rejected = manager->submit(malformed_owner, "fixture", malformed_connection.central,
            {control::Kind::Request, detail::message_bus_domain, old_operation,
             malformed_connection.next_request++, prefix});
        check(!rejected && rejected.error().is(bus::ErrorCode::Protocol), "old option wire is never reinterpreted");
    }
    // Every handle is local and idle before simulating confirmed owner exit.
    manager->exited(owner);
    manager->exited(malformed_owner);
    ok(manager->drain_until(deadline()));
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
                control::put_u32(payload, detail::publisher_options_mask);
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

void test_notification_topology() {
    Connection writer;
    Connection reader;
    auto manager = take(detail::BusManager::create());
    const auto name = detail::shared_name('n');
    const auto subscribe = [&](const std::string& topic) {
        control::Bytes payload;
        ok(control::put_string(payload, topic));
        control::put_u64(payload, Frame::schema().hash);
        control::put_u32(payload, 0);
        auto response = take(manager_request(*manager, reader, 2, detail::Subscribe, std::move(payload), name));
        control::Reader decoded(response);
        const auto id = decoded.u64();
        const auto count = decoded.u32();
        for (std::uint32_t index = 0; index < count; ++index) {
            (void)detail::decode_attachment(decoded);
        }
        check(decoded.done(), "subscription response consumed");
        return id;
    };
    const auto mutate = [&](std::uint16_t operation, std::uint64_t id) {
        control::Bytes payload;
        control::put_u64(payload, id);
        take(manager_request(*manager, reader, 2, operation, std::move(payload), name));
    };
    const auto route_event = [&](std::uint64_t publisher, std::uint32_t count) {
        const auto event = receive_single_change(*manager, writer, 1);
        check(event.operation == detail::NotificationRoutesChanged, "dedicated route event");
        control::Reader decoded(event.payload);
        check(decoded.u64() == publisher && decoded.u32() == count, "route belongs to the current Publisher");
        if (count) {
            check(decoded.u64() == 2 && decoded.string() == name, "route uses central-approved generation/name");
        }
        check(decoded.done(), "one destination per Module, not per subscription");
    };
    const auto publication = register_publisher(*manager, writer, 1, "routes");
    const auto first = subscribe("routes");
    mutate(detail::EnableNotifications, first);
    route_event(publication.id, 1);
    const auto second = subscribe("routes");
    mutate(detail::EnableNotifications, second);
    const auto get_last_only = subscribe("routes");
    mutate(detail::Unsubscribe, first);
    mutate(detail::Unsubscribe, second);
    // Same-session FIFO: any redundant route update from the intermediate
    // enable/close would be received here ahead of this empty destination set.
    route_event(publication.id, 0);
    mutate(detail::Unsubscribe, get_last_only);
    const auto early = subscribe("sub-first");
    mutate(detail::EnableNotifications, early);
    const auto early_publication = register_publisher(*manager, writer, 1, "sub-first", 1);
    auto early_ring = take(detail::RingBuffer::open(early_publication, false));
    detail::shared_store(early_ring->reader(0).token, 1);
    check(receive_single_change(*manager, reader, 2).operation == detail::Attach,
        "subscriber-first route accompanies ordinary source attachment");
    arm(FaultPoint::NotificationRoutesEncode);
    manager->exited(2);
    ok(manager->drain_until(deadline()));
    check(fault_hit.load(), "route encoding failure injected during owner cleanup");
    check(detail::shared_load(early_ring->reader(0).token) == 0,
          "notification failure cannot prevent confirmed-exit reader cleanup");
    manager->exited(1);
    ok(manager->drain_until(deadline()));
}

void test_notification_capacity() {
    Connection connection;
    bus::Limits limits;
    limits.subscribers_per_topic = detail::maximum_ring_buffer_readers;
    auto manager = take(detail::BusManager::create(limits));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> registrations;
    bool rejected = false;
    const auto request_id = [&](std::uint64_t owner, std::uint16_t operation, std::uint64_t id) {
        control::Bytes body;
        control::put_u64(body, id);
        return manager_request(*manager, connection, owner, operation, std::move(body), std::string(24, 'n'));
    };
    // Reuse the test transport for many authenticated owner identities. No native
    // endpoints or child processes are needed to test central wire admission.
    for (std::uint64_t owner = 1; owner <= limits.subscribers_per_topic; ++owner) {
        control::Bytes body;
        ok(control::put_string(body, "bounded-routes"));
        control::put_u64(body, Frame::schema().hash);
        control::put_u32(body, 0);
        auto response = take(manager_request(*manager, connection, owner, detail::Subscribe,
            std::move(body), std::string(24, 'n')));
        control::Reader decoded(response);
        const auto id = decoded.u64();
        check(decoded.u32() == 0 && decoded.done(), "empty topic subscriber");
        auto enabled = request_id(owner, detail::EnableNotifications, id);
        if (!enabled) {
            check(enabled.error().is(bus::ErrorCode::CapacityExceeded), "oversized route list is rejected before mutation");
            const auto [first_owner, first_id] = registrations.front();
            take(request_id(first_owner, detail::Unsubscribe, first_id));
            take(request_id(owner, detail::EnableNotifications, id));
            rejected = true;
            registrations.emplace_back(owner, id);
            break;
        }
        registrations.emplace_back(owner, id);
    }
    check(rejected, "route wire ceiling reached before reader ceiling");
    for (const auto& [owner, id] : registrations) {
        manager->exited(owner);
    }
    ok(manager->drain_until(deadline()));
}

void test_release_confirmation(bool break_session) {
    Connection connection;
    std::vector<bus::Subscriber<Frame>> subscribers;
    for (std::size_t index = 0; index <= detail::maximum_releases_per_batch; ++index) {
        auto subscribing = std::async(std::launch::async, [&] {
            return connection.client->subscribe<Frame>("release-order");
        });
        connection.reply(take(connection.central->receive_until(deadline())), subscription(index + 1));
        subscribers.push_back(take(subscribing.get()));
    }

    auto last = std::move(subscribers.back());
    subscribers.pop_back();
    subscribers.clear();
    auto closing = std::async(std::launch::async, [&] { return last.close(); });
    auto first = take(connection.central->receive_until(deadline()));
    control::Reader reader(first.payload);
    check(first.operation == detail::ReleaseBatch && reader.u32() == detail::maximum_releases_per_batch,
          "clear sends one bounded batch before explicit close");
    // Container element destruction order is unspecified. Each retired identity appears exactly once.
    std::array<bool, detail::maximum_releases_per_batch> seen{};
    for (std::size_t index = 0; index < seen.size(); ++index) {
        check(reader.u32() == detail::Unsubscribe, "release kind");
        const auto id = reader.u64();
        check(id > 0 && id <= seen.size() && !seen[id - 1], "only cleared identities in the first batch");
        seen[id - 1] = true;
    }
    check(reader.done(), "no trailing fields");
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    check(closing.wait_for(0s) != std::future_status::ready, "a write to the socket is not completion");

    if (break_session) {
        detail::RuntimeAccess::shutdown(*connection.client);
        const auto result = closing.get();
        check(!result && result.error().is(bus::ErrorCode::Closed), "disconnect wakes unconfirmed close");
        return;
    }

    connection.reply(first, {});
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto second = take(connection.central->receive_until(deadline()));
    control::Reader tail(second.payload);
    check(second.operation == detail::ReleaseBatch && second.request > first.request &&
          tail.u32() == 1 && tail.u32() == detail::Unsubscribe &&
          tail.u64() == seen.size() + 1 && tail.done(), "later close follows clear exactly once");
    check(closing.wait_for(0s) != std::future_status::ready, "close waits for its own batch");
    connection.reply(second, {});
    ok(closing.get());
    auto repeated = last.close();
    check(!repeated && repeated.error().is(bus::ErrorCode::Closed), "closed handle cannot replay release");
}

void test_release_backpressure() {
    Connection connection(true);
    constexpr std::uint64_t id = 1;
    auto subscribing = std::async(std::launch::async, [&] {
        return connection.client->subscribe<Frame>("backpressure");
    });
    connection.reply(take(connection.central->receive_until(deadline())), subscription(id));
    std::optional handle{take(subscribing.get())};
    handle.reset();

    ok(detail::RuntimeAccess::progress_control(*connection.client));
    check(!take(connection.central->try_receive()), "rejected send never reached the wire");
    connection.expect_release(detail::Unsubscribe, id);
    check(connection.module->state() == control::State::Open, "retry keeps session open");
}

void test_release_fence() {
    Connection connection;
    auto subscribe = [&](std::uint64_t id) {
        auto call = std::async(std::launch::async, [&] {
            return connection.client->subscribe<Frame>("fence");
        });
        connection.reply(take(connection.central->receive_until(deadline())), subscription(id));
        return take(call.get());
    };
    std::optional first{subscribe(1)};
    std::optional second{subscribe(2)};
    first.reset();
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto batch = take(connection.central->receive_until(deadline()));
    std::promise<void> waiting;
    auto entered = waiting.get_future();
    auto query = std::async(std::launch::async, [&] {
        // MSVC can reuse an async worker. Remove the hook before the promise leaves scope.
        struct ResetHook {
            ~ResetHook() {
                detail::testing::before_release_wait = {};
            }
        } reset;
        detail::testing::before_release_wait = [&] { waiting.set_value(); };
        return connection.client->list_topic_connections({}, deadline());
    });
    check(entered.wait_until(deadline()) == std::future_status::ready, "query reached release fence");
    second.reset();
    connection.reply(batch, {});

    // Leave the later release unconfirmed. An older query must already be allowed to send.
    auto request = take(connection.central->receive_until(deadline()));
    check(request.operation == detail::ListTopicConnections, "new retirements cannot extend earlier fence");
    connection.reply(request, take(detail::encode_connections({})));
    (void)take(query.get());
    connection.expect_release(detail::Unsubscribe, 2);
}

// The only application traffic on the paused reader Session is topology. Pace
// the writer on independent registration replies so the reader's own queue fills.
void test_paused_topology(bool attaching, std::size_t topic_count, bool mutate_snapshot = false, bool exhaust_history = false) {
    Connection writer;
    Connection reader;
    auto manager = take(detail::BusManager::create());
    constexpr std::uint64_t writer_mid = 1;
    constexpr std::uint64_t reader_mid = 2;
    std::vector<bus::Subscriber<Frame>> subscriptions;
    std::vector<std::uint64_t> publisher_ids;
    std::vector<std::string> topics;
    for (std::size_t index = 0; index < topic_count; ++index) {
        auto topic = "pressure/topic" + std::to_string(index);
        if (!attaching) {
            publisher_ids.push_back(register_publisher(*manager, writer, writer_mid, topic).id);
        }

        auto subscribed = client_request(*manager, reader, reader_mid, [&] {
            return reader.client->subscribe<Frame>(topic);
        });
        subscriptions.push_back(take(std::move(subscribed)));
        topics.push_back(std::move(topic));
    }

    auto sources = [&] {
        std::set<std::uint64_t> ids;
        for (const auto& source : detail::RuntimeAccess::inspect_attachments(*reader.client)) {
            ids.insert(source.descriptor.id);
        }
        return ids;
    };
    check(sources() == std::set(publisher_ids.begin(), publisher_ids.end()), "unexpected starting topology");

    // Hold back application reads across the entire burst; the socket and its
    // heartbeat worker stay live. Each command below has its own central ACK.
    for (std::size_t index = 0; index < topic_count; ++index) {
        if (attaching) {
            publisher_ids.push_back(register_publisher(*manager, writer, writer_mid, topics[index]).id);
        } else {
            control::Bytes release;
            control::put_u32(release, 1);
            control::put_u32(release, detail::ClosePublisher);
            control::put_u64(release, publisher_ids[index]);
            take(manager_request(*manager, writer, writer_mid, detail::ReleaseBatch, std::move(release)));
        }
    }

    // A reply on the writer socket is not a delivery fence on the reader socket.
    // Keep the dispatcher paused for a bounded slow-reader interval; the reactor
    // continues to receive frames and exchange heartbeats throughout.
    constexpr auto slow_reader_interval = 1s;
    const auto resume_at = control::Clock::now() + slow_reader_interval;
    while (reader.module->state() == control::State::Open && control::Clock::now() < resume_at) {
        std::this_thread::sleep_until((std::min)(resume_at, control::Clock::now() + heartbeat_interval));
    }

    std::set<std::uint64_t> expected = attaching
        ? std::set(publisher_ids.begin(), publisher_ids.end()) : std::set<std::uint64_t>{};
    if (mutate_snapshot) {
        // A Publisher with no local reader must not enter this Host's snapshot.
        register_publisher(*manager, writer, writer_mid, "unrelated/not-visible");
    }
    const auto end = deadline();
    std::size_t delivered = 0;
    bool mutated = false;
    bool saw_snapshot = false;
    std::uint64_t target_revision = topic_count;
    std::set<std::uint64_t> snapshot_sources;
    std::uint64_t snapshot_serial = 0;
    bool replaced = false;
    while (sources() != expected) {
        auto frame = reader.module->receive_until(end);
        if (!frame) {
            std::cerr << (attaching ? "Attach" : "Detach") << " changes=" << topic_count
                      << " delivered=" << delivered << " reader=" << error_to_string(frame.error()) << '\n';
            throw std::runtime_error("paused Host failed before its topology converged");
        }

        auto page = take(detail::decode_topology_page(frame->payload));
        const bool staged_snapshot = page.mode == detail::TopologyMode::Snapshot && !page.final;
        saw_snapshot |= page.mode == detail::TopologyMode::Snapshot;
        if (mutate_snapshot && page.mode == detail::TopologyMode::Snapshot) {
            if (!page.id.index) {
                replaced = snapshot_serial != 0;
                snapshot_serial = page.id.transfer;
                snapshot_sources.clear();
            }
            ok(detail::visit_topology_records(page.records, [&](auto kind, auto body) -> Result<void> {
                if (kind == detail::Attach) {
                    auto group = take(detail::decode_source_group(body));
                    snapshot_sources.insert(group.publisher.id);
                }
                return {};
            }));
        }
        auto before = sources();
        ok(detail::RuntimeAccess::receive(*reader.client, std::move(*frame)));
        if (staged_snapshot) {
            check(sources() == before, "incomplete snapshot leaked into a Subscriber");
        }
        auto ack = take(reader.central->receive_until(end));
        check(ack.operation == detail::SyncAck, "Host acknowledges applied or staged topology");
        if (mutate_snapshot && staged_snapshot && !mutated) {
            mutated = true;
            auto closed = client_request(*manager, reader, reader_mid, [&] { return subscriptions.back().close(); });
            ok(std::move(closed));
            expected.erase(publisher_ids.back());

            // Register a new reader after the cut, with its own initial sources.
            subscriptions.push_back(take(client_request(*manager, reader, reader_mid, [&] {
                return reader.client->subscribe<Frame>("unrelated/not-visible");
            })));
            for (auto id : sources()) {
                expected.insert(id);
            }

            control::Bytes release;
            control::put_u32(release, 1);
            control::put_u32(release, detail::ClosePublisher);
            const auto removed = publisher_ids[std::ranges::max_element(topics) - topics.begin()];
            control::put_u64(release, removed);
            if (exhaust_history) {
                arm(FaultPoint::SnapshotHistory);
            }
            take(manager_request(*manager, writer, writer_mid, detail::ReleaseBatch, std::move(release)));
            expected.erase(removed);
            ++target_revision;
        }
        ok(manager->submit(reader_mid, "fixture", reader.central, std::move(ack)));
        ++delivered;
    }
    ok(manager->drain_until(end));
    auto status = manager->topology_status(reader_mid);
    check(replaced == exhaust_history, "replace only the snapshot whose undo storage failed");
    if (mutate_snapshot && !exhaust_history) {
        check(snapshot_sources == std::set(publisher_ids.begin(), publisher_ids.end()),
              "frozen snapshot leaked an unrelated source or lost a subsequently retired Publisher");
    }
    check(!mutate_snapshot || mutated, "mutation must occur between two snapshot pages");
    check(topic_count <= config::message_bus_topology_history_records || saw_snapshot, "overflow must use a snapshot");
    check(status && status->target == target_revision && status->applied == status->target,
          "same generation reached the final revision without more registrations");
    check(reader.module->state() == control::State::Open, "reader lost its Session");
}

void test_topology_admission_retry() {
    Connection writer;
    Connection reader;
    auto manager = take(detail::BusManager::create());
    std::atomic<bool> reject_receipt{true};
    detail::ControlTransport transport;
    transport.generation = 2;
    transport.send = [&](control::Frame frame) -> Result<void> {
        if (frame.operation == detail::SyncAck && reject_receipt.exchange(false)) {
            return std::unexpected(Error{control::ErrorCode::Backpressure, control::ErrorOperation::Send});
        }
        return reader.module->send(std::move(frame));
    };
    transport.close = [&] { reader.module->close(); };
    reader.client = take(detail::RuntimeAccess::create(std::move(transport)));
    auto sub = take(client_request(*manager, reader, 2, [&] { return reader.client->subscribe<Frame>("admission"); }));
    reject_page.store(true);
    auto pub = register_publisher(*manager, writer, 1, "admission");
    // No further command reaches the Bus. Its worker must retry the rejected page.
    auto page = take(reader.module->receive_until(deadline()));
    check(!reject_page.load() && page.operation == detail::SyncPage, "central retries without another change");
    ok(detail::RuntimeAccess::receive(*reader.client, std::move(page)));
    const auto sources = detail::RuntimeAccess::inspect_attachments(*reader.client);
    check(!reject_receipt.load() && sources.size() == 1 && sources.front().descriptor.id == pub.id,
          "Host committed before its first receipt was rejected");
    auto status = manager->topology_status(2);
    check(status && status->applied == 0 && status->target == 1, "missed receipt is visible as lag");

    // This is the existing watchdog operation, independent of the user's pump.
    ok(detail::RuntimeAccess::progress_control(*reader.client));
    auto ack = take(reader.central->receive_until(deadline()));
    ok(manager->submit(2, "reader", reader.central, std::move(ack)));
    ok(manager->drain_until(deadline()));
    status = manager->topology_status(2);
    check(status && status->applied == 1 && reader.module->state() == control::State::Open,
          "rejected receipt eventually confirmed on the original session");
}

void test_request_after_release_ack(bool expire) {
    std::atomic<unsigned> admitted{0};
    Deadline admission_time{};
    Deadline waited_until{};
    Connection connection;
    detail::ControlTransport observed;
    observed.send = [&](control::Frame frame) {
        const bool subscribing = frame.operation == detail::Subscribe;
        const auto sending_at = control::Clock::now();
        auto result = connection.module->send(std::move(frame));
        if (subscribing && result) {
            admission_time = sending_at;
            ++admitted;
        }
        return result;
    };
    observed.close = [&] { connection.module->close(); };
    connection.client = take(detail::RuntimeAccess::create(std::move(observed)));

    auto subscribe_with_reply = [&](std::uint64_t id) {
        auto call = std::async(std::launch::async, [&] { return connection.client->subscribe<Frame>("expiry"); });
        connection.reply(take(connection.central->receive_until(deadline())), subscription(id));
        return take(call.get());
    };
    std::optional first{subscribe_with_reply(1)};
    first.reset();
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto batch = take(connection.central->receive_until(deadline()));
    check(batch.operation == detail::ReleaseBatch, "expected a real release request");

    std::promise<void> waiting;
    auto entered = waiting.get_future();
    auto next = std::async(std::launch::async, [&] {
        struct ResetHooks {
            ~ResetHooks() {
                detail::testing::before_release_wait = {};
                detail::testing::after_release_wait = {};
            }
        } reset;
        detail::testing::before_release_wait = [&] { waiting.set_value(); };
        if (expire) {
            detail::testing::after_release_wait = [&](Deadline end) {
                // Model a descheduled caller after a real ACK, using its actual request deadline.
                waited_until = end;
                std::this_thread::sleep_until(end);
            };
        }
        return connection.client->subscribe<Frame>("after-ack");
    });
    check(entered.wait_until(deadline()) == std::future_status::ready, "caller did not reach the release fence");
    connection.reply(batch, {});
    if (!expire) {
        connection.reply(take(connection.central->receive_until(deadline())), subscription(2));
        (void)take(next.get());
        check(admitted == 2, "timely ACK must admit exactly one new Subscribe");
        return;
    }

    auto result = next.get();
    check(waited_until != Deadline{} && control::Clock::now() >= waited_until, "late-resume fixture did not run");
    const bool resumed_expired = !result && result.error().is(bus::ErrorCode::Timeout)
        && result.error().is_operation(bus::ErrorOperation::Request);
    check(resumed_expired, "expected Request timeout after release ACK");
    std::cerr << "expired Subscribe admissions=" << admitted.load() - 1
              << " session_open=" << (connection.module->state() == control::State::Open) << '\n';
    if (admitted > 1) {
        check(admission_time >= waited_until, "unexpected Subscribe before the requested delay");
    }
    check(admitted == 1, "transport accepted a new Subscribe after its deadline");
    check(connection.module->state() == control::State::Open, "unsubmitted timeout closed the Session");
    auto retry = std::async(std::launch::async, [&] { return connection.client->subscribe<Frame>("retry"); });
    auto request = take(connection.central->receive_until(deadline()));
    check(request.operation == detail::Subscribe && request.request == batch.request + 1,
          "expired request consumed an ID or sent an unexpected frame");
    connection.reply(request, subscription(2));
    (void)take(retry.get());
}

void test_missing_release_ack() {
    Connection connection;
    auto call = std::async(std::launch::async, [&] { return connection.client->subscribe<Frame>("release"); });
    connection.reply(take(connection.central->receive_until(deadline())), subscription(1));
    std::optional first{take(call.get())};
    first.reset();
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto batch = take(connection.central->receive_until(deadline()));
    check(batch.operation == detail::ReleaseBatch, "expected a pending release batch");

    constexpr auto query_budget = 100ms;
    auto result = connection.client->list_topic_connections({}, control::Clock::now() + query_budget);
    check(!result && result.error().is(bus::ErrorCode::Io) &&
          result.error().is_operation(bus::ErrorOperation::ListTopicConnections), "expected a query failure");
    const Error timeout{bus::ErrorCode::Timeout, bus::ErrorOperation::CloseRegistration};
    const auto causes = result.error().causes();
    check(!causes.empty() && causes.front() == timeout.record(), "expected a release wait timeout");

    connection.reply(batch, {});
    auto retry = std::async(std::launch::async, [&] { return connection.client->list_topic_connections({}, deadline()); });
    auto request = take(connection.central->receive_until(deadline()));
    check(request.request == batch.request + 1 && request.operation == detail::ListTopicConnections,
          "waiting for a missing ACK consumed a request ID");
    connection.reply(request, take(detail::encode_connections({})));
    (void)take(retry.get());
}

void test_submitted_mutation_timeout() {
    Connection connection;
    auto call = std::async(std::launch::async, [&] { return connection.client->subscribe<Frame>("unknown"); });
    auto admitted = take(connection.central->receive_until(deadline()));
    check(admitted.operation == detail::Subscribe, "central did not receive the mutation");
    // Withhold the reply after receipt. The caller cannot tell whether the registration took effect.
    auto result = call.get();
    check(!result && result.error().is(bus::ErrorCode::Timeout) &&
          result.error().is_operation(bus::ErrorOperation::Request), "expected a submitted mutation timeout");
    connection.expect_closed();
}

// BusManager owns the worker that calls this journal. Only inspect or arm it
// between requests, after drain_until has synchronized with that worker.
struct PublisherJournal final : vinc::detail::ResourceNames {
    std::set<std::string> records;
    std::string occupied_name;
    unsigned collide_on = 0;
    unsigned allocations = 0;
    unsigned reject_reserve = 0;
    bool throw_on_reserve = false;
    bool reject_retire = false;
    std::vector<std::string> retired;
    const Error journal_error{osal::ErrorCode::PermissionDenied};

    Result<std::string> reserve() override {
        ++allocations;
        if (allocations == reject_reserve) {
            if (throw_on_reserve) {
                throw std::bad_alloc();
            }
            return std::unexpected(journal_error);
        }
        auto name = allocations == collide_on ? occupied_name : detail::shared_name('r');
        check(records.insert(name).second, "duplicate reservation in test journal");
        return name;
    }

    Result<void> retire(std::string_view name) override {
        retired.emplace_back(name);
        if (reject_retire) {
            return std::unexpected(journal_error);
        }
        check(records.erase(std::string(name)) == 1, "retiring an unknown test reservation");
        return {};
    }
};

void test_publisher_reservation(unsigned collide_on, bool reject_retire = false) {
    Connection connection;
    auto journal = std::make_shared<PublisherJournal>();
    auto manager = take(detail::BusManager::create({}, journal));
    constexpr std::uint64_t original_owner = 1;
    constexpr std::uint64_t new_owner = 2;
    const std::string topic = "reservation/existing";
    (void)register_publisher(*manager, connection, original_owner, topic);
    ok(manager->drain_until(deadline()));
    const auto baseline = journal->records;
    journal->allocations = 0;
    journal->collide_on = collide_on;
    journal->reject_retire = reject_retire;
    journal->occupied_name = detail::shared_name('x');
    auto foreign = take(detail::NamedCounter::create(journal->occupied_name));
    check(take(foreign->next()) == 1, "initialize independent mapping");

    Connection other;
    auto result = manager_request(*manager, other, new_owner, detail::Advertise, advertise_payload(topic));
    ok(manager->drain_until(deadline()));
    if (collide_on == 0) {
        (void)take(std::move(result));
        manager->exited(new_owner);
        ok(manager->drain_until(deadline()));
    } else {
        const auto expected = collide_on == 1 ? bus::ErrorOperation::CreateControl : bus::ErrorOperation::CreatePayload;
        check(!result && result.error().is_operation(expected), "failure at wrong ring creation step");
        const auto causes = result.error().causes();
        check(!causes.empty() && causes.back().domain == ErrorDomain::Osal &&
              causes.back().code == static_cast<int>(osal::ErrorCode::AlreadyExists), "missing native name collision");
        auto reopened = take(detail::NamedCounter::open(journal->occupied_name));
        check(take(reopened->next()) == 2, "failure damaged the foreign mapping");
        std::cerr << error_to_string(result.error()) << " baseline=" << baseline.size()
                  << " current=" << journal->records.size() << '\n';
    }
    auto diagnostic = manager->last_registration_cleanup_failure();
    if (reject_retire) {
        check(diagnostic && diagnostic->primary == result.error() && diagnostic->control && diagnostic->payload,
              "lost independent Publisher rollback diagnostics");
        auto retained = baseline;
        retained.insert(diagnostic->control->resource_name);
        retained.insert(diagnostic->payload->resource_name);
        check(journal->records == retained && journal->retired.size() == 2 &&
              diagnostic->payload->resource_name == journal->occupied_name &&
              diagnostic->control->cleanup == journal->journal_error && diagnostic->payload->cleanup == journal->journal_error,
              "journal failures must preserve exactly both names");
        journal->reject_retire = false;
    } else {
        check(!diagnostic && journal->records == baseline, "Publisher left its shared memory names reserved");
    }
}

// Hold an authentic Detach in the application queue while the ring has no
// remaining handles. This isolates the lazy-open race from queue overflow.
template<bool Images = false>
void test_retired_source_before_dispatch(bool with_other_publisher) {
    Connection writer;
    Connection reader;
    Connection live_writer;
    auto manager = take(detail::BusManager::create());
    constexpr std::uint64_t writer_owner = 1;
    constexpr std::uint64_t reader_owner = 2;
    const std::string topic = "retirement/lazy-open";
    register_publisher(*manager, writer, writer_owner, topic);
    std::optional<bus::Publisher<Frame>> live;
    if (with_other_publisher) {
        live.emplace(take(client_request(*manager, live_writer, 3, [&] {
            return live_writer.client->advertise<Frame>(topic, {.slots = fixture_slots, .max_message_bytes = message_bytes});
        })));
        auto value = make_message<Frame>();
        value->data().sequence = 42;
        ok(live->push(*value));
    }
    auto subscribed = take(client_request(*manager, reader, reader_owner, [&] {
        if constexpr (Images) {
            return detail::ImageAccess::subscribe(*reader.client, topic, Frame::schema().hash);
        } else {
            return reader.client->subscribe<Frame>(topic);
        }
    }));
    manager->exited(writer_owner);
    ok(manager->drain_until(deadline()));
    auto detach = take(reader.module->receive_until(deadline()));
    // No control dispatcher is running. Waiting for central here would time out.
    auto core = detail::DispatchAccess::core(subscribed);
    auto captured = take(detail::capture(core));
    check(captured.size() == (live ? 1u : 0u), "pump includes only usable sources before Detach");
    const auto last = subscribed.get_last();
    if (live) {
        check(bool(last), "retirement must not hide another Publisher");
        if constexpr (Images) {
            check(take(Frame::from_bytes((*last)->image.bytes()))->data().sequence.get() == 42,
                  "retirement changed another Publisher image");
        } else {
            check((*last)->data().sequence == 42, "retirement changed another Publisher message");
        }
        check(captured.front().upper == 1, "live publication is dispatchable in this pump");
        if constexpr (Images) {
            auto delivered = take(detail::read_image_batch(core, captured));
            check(take(Frame::from_bytes(delivered->image.bytes()))->data().sequence.get() == 42,
                  "image pump delivers live data before confirmation");
            auto exhausted = detail::read_image_batch(core, captured);
            check(!exhausted && exhausted.error().is(bus::ErrorCode::NoNewData), "image pump finishes its batch");
        } else {
            const auto factory = [](message::detail::Binding binding) -> std::unique_ptr<BaseMessage> {
                return std::unique_ptr<Frame>(new Frame(message::detail::RecordAccess::bind<Frame>(std::move(binding))));
            };
            auto delivered = take(detail::read_batch(core, captured, Frame::schema(), factory));
            check(static_cast<Frame&>(*delivered).data().sequence == 42, "pump delivers live data before confirmation");
            auto exhausted = detail::read_batch(core, captured, Frame::schema(), factory);
            check(!exhausted && exhausted.error().is(bus::ErrorCode::NoNewData), "pump finishes its batch");
        }
    } else {
        check(!last && last.error().is(bus::ErrorCode::NoPublishedData), "no visible publication before Detach");
    }
    ok(detail::RuntimeAccess::progress_control(*reader.client));
    auto check_request = take(reader.central->receive_until(deadline()));
    check(check_request.operation == detail::CheckSource, "Host must check a missing mapping asynchronously");
    ok(manager->submit(reader_owner, "rollback-owner", reader.central, std::move(check_request)));
    auto confirmation = take(reader.module->receive_until(deadline()));
    if (live) {
        ok(detail::RuntimeAccess::receive(*reader.client, std::move(detach)));
        ok(detail::RuntimeAccess::receive(*reader.client, std::move(confirmation)));
    } else {
        ok(detail::RuntimeAccess::receive(*reader.client, std::move(confirmation)));
        check(detail::RuntimeAccess::inspect_attachments(*reader.client).empty(), "central confirmation removes stale source");
        ok(detail::RuntimeAccess::receive(*reader.client, std::move(detach)));
    }
    const auto after = subscribed.get_last();
    check(live ? bool(after) : (!after && after.error().is(bus::ErrorCode::NoPublishedData)), "retirement after dispatch");
}

template<bool Images = false>
void test_confirmed_missing_source(bool repair_mapping) {
    Connection connection;
    const auto source = descriptor();
    auto pending = std::async(std::launch::async, [&] {
        if constexpr (Images) {
            return detail::ImageAccess::subscribe(*connection.client, "missing/confirmed", Frame::schema().hash);
        } else {
            return connection.client->subscribe<Frame>("missing/confirmed");
        }
    });
    constexpr std::uint64_t subscription_id = 77;
    connection.reply(take(connection.central->receive_until(deadline())), subscription(subscription_id, &source));
    auto sub = take(pending.get());
    const auto first = sub.get_last();
    check(!first && first.error().is(bus::ErrorCode::NoPublishedData), "first missing mapping is pending");
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto request = take(connection.central->receive_until(deadline()));
    control::Reader fields(request.payload);
    check(request.operation == detail::CheckSource && fields.string() == "missing/confirmed" &&
          fields.u64() == subscription_id && fields.u64() == source.id && fields.done(), "verify the exact relationship");
    ok(detail::RuntimeAccess::progress_control(*connection.client));
    auto capture = take(detail::capture(detail::DispatchAccess::core(sub)));
    check(capture.empty(), "pump must finish while confirmation is withheld");
    std::shared_ptr<detail::RingBuffer> repaired;
    if (repair_mapping) {
        repaired = take(detail::RingBuffer::create(source));
        check(take(detail::capture(detail::DispatchAccess::core(sub))).empty(),
              "repeated pump calls must not retry the native open before confirmation");
    }
    control::Bytes body;
    control::put_u32(body, 1);
    connection.reply(request, std::move(body));
    auto retry = detail::capture(detail::DispatchAccess::core(sub));
    if (repair_mapping) {
        check(retry && retry->size() == 1 && retry->front().publisher == source.id, "confirmed mapping can recover");
    } else {
        check(!retry && retry.error().is_operation(bus::ErrorOperation::OpenControl) && !retry.error().causes().empty()
              && retry.error().causes().back().domain == ErrorDomain::Osal
              && retry.error().causes().back().code == static_cast<std::int32_t>(osal::ErrorCode::NotFound),
              "confirmed missing source retains its native error");
    }
}

void test_second_ring_reserve(bool inject_exception) {
    auto journal = std::make_shared<PublisherJournal>();
    journal->reject_reserve = 2;
    journal->throw_on_reserve = inject_exception;
    detail::RegistrationCleanupFailure cleanup;
    bool threw = false;
    try {
        auto result = detail::RingBuffer::create(descriptor(), journal, cleanup);
        check(!result && result.error().is_operation(bus::ErrorOperation::Advertise) && !result.error().causes().empty() &&
              result.error().causes().back() == journal->journal_error.record(), "lost second reserve failure");
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    check(threw == inject_exception && journal->allocations == 2 && journal->records.empty() &&
          journal->retired.size() == 1 && !cleanup.control && !cleanup.payload, "first reserve rollback");
}

#ifdef __linux__
// Select control (first call) or payload (second). When cleanup is denied,
// verify the actual POSIX name survives for recovery and clean up the fixture.
void test_native_ring_rollback(bool fail_map, unsigned allocation, bool deny_unlink) {
    auto journal = std::make_shared<PublisherJournal>();
    journal->collide_on = allocation;
    journal->occupied_name = detail::shared_name('f');
    if (fail_map) {
        ring_maps_before_failure = allocation;
    } else {
        ring_sizes_before_failure = allocation;
    }
    if (deny_unlink) {
        ring_unlink_denied = journal->occupied_name;
    }
    detail::RegistrationCleanupFailure cleanup;
    auto result = detail::RingBuffer::create(descriptor(), journal, cleanup);
    ring_unlink_denied = {};
    const auto expected_map = allocation == 1 ? bus::ErrorOperation::MapControl : bus::ErrorOperation::MapPayload;
    const auto expected_create = allocation == 1 ? bus::ErrorOperation::CreateControl : bus::ErrorOperation::CreatePayload;
    check(!result && result.error().is_operation(fail_map ? expected_map : expected_create) && !result.error().causes().empty() &&
          result.error().causes().back().native.value() == (fail_map ? ENOMEM : ENOSPC), "lost primary ring failure");
    auto reopened = osal::SharedMemory::open(journal->occupied_name, osal::SharedMemoryAccess::ReadOnly);
    if (deny_unlink) {
        const auto& failure = allocation == 1 ? cleanup.control : cleanup.payload;
        const auto& other = allocation == 1 ? cleanup.payload : cleanup.control;
        check(reopened && failure && !other && failure->resource_name == journal->occupied_name &&
              failure->cleanup.native.value() == EACCES && journal->records == std::set{journal->occupied_name} &&
              journal->retired.size() == 1, "uncertain cleanup must retain only its own name");
        ok(osal::SharedMemory::unlink_name(journal->occupied_name));
    } else {
        check(!reopened && reopened.error().is(osal::ErrorCode::NotFound) && journal->records.empty() &&
              journal->retired.size() == 2 && !cleanup.control && !cleanup.payload, "confirmed ring rollback");
    }
    for (const auto& name : journal->retired) {
        auto retired = osal::SharedMemory::open(name, osal::SharedMemoryAccess::ReadOnly);
        check(!retired && retired.error().is(osal::ErrorCode::NotFound), "journal retired a surviving native name");
    }
}
#endif

void test_batch_ownership() {
    Connection owner;
    Connection other;
    auto manager = take(detail::BusManager::create());
    const auto publication = register_publisher(*manager, owner, 1, "batch/first");
    const auto foreign = register_publisher(*manager, other, 2, "batch/other");
    auto release = [&](std::initializer_list<std::uint64_t> ids) {
        control::Bytes body;
        control::put_u32(body, static_cast<std::uint32_t>(ids.size()));
        for (const auto id : ids) {
            control::put_u32(body, detail::ClosePublisher);
            control::put_u64(body, id);
        }

        return manager_request(*manager, owner, 1, detail::ReleaseBatch, std::move(body));
    };
    auto duplicate = release({publication.id, publication.id});
    check(!duplicate && duplicate.error().is(bus::ErrorCode::Protocol), "reject duplicate release");
    auto crossed = release({publication.id, foreign.id});
    check(!crossed && crossed.error().is(bus::ErrorCode::Protocol), "reject foreign release");
    // Both rejected batches began with a valid ID; it must still be owned and releasable.
    (void)take(release({publication.id}));
    auto ring = take(detail::RingBuffer::open(foreign, false));
    check(ring->alive(), "other owner retains live ring");
    manager->exited(2);
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
    auto close_subscriber = std::async(std::launch::async, [&] { return subscriber.close(); });
    process_release(*manager, subscriber_connection, subscriber_owner, detail::Unsubscribe);
    ok(close_subscriber.get());
    auto close_publisher = std::async(std::launch::async, [&] { return publisher.close(); });
    process_release(*manager, publisher_connection, publisher_owner, detail::ClosePublisher);
    ok(close_publisher.get());
    manager->exited(publisher_owner);
    manager->exited(subscriber_owner);
    ok(manager->drain_until(deadline()));
}

void test_readonly_timeout() {
    Connection connection;
    constexpr auto query_budget = std::chrono::milliseconds{50};
    auto call = std::async(std::launch::async, [&] {
        return connection.client->list_topic_connections({}, control::Clock::now() + query_budget);
    });
    auto request = take(connection.central->receive_until(deadline()));
    auto expired = call.get();
    check(!expired, "read-only query times out");
    check(connection.module->state() == control::State::Open, "query timeout retains control session");
    connection.reply(request, take(detail::encode_connections({})));
    auto retry = std::async(std::launch::async, [&] {
        return connection.client->list_topic_connections({}, deadline());
    });
    auto next = take(connection.central->receive_until(deadline()));
    connection.reply(next, take(detail::encode_connections({})));
    check(take(retry.get()).connections.empty(), "late reply retired; subsequent query succeeds");
    constexpr auto retirement_budget = std::chrono::milliseconds{30};
    for (std::size_t index = 0; index <= config::message_bus_pending_requests; ++index) {
        auto dropped = connection.client->list_topic_connections({}, control::Clock::now() + retirement_budget);
        check(!dropped, "lost reply times out");
        take(connection.central->receive_until(deadline()));
    }
    auto recovered = std::async(std::launch::async, [&] {
        return connection.client->list_topic_connections({}, deadline());
    });
    auto recovered_frame = take(connection.central->receive_until(deadline()));
    connection.reply(recovered_frame, take(detail::encode_connections({})));
    check(take(recovered.get()).connections.empty(), "lost replies never exhaust pending capacity");
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
    ok(ring->publish(reservation, detail::minimum_message_bytes, 1, 1, 1));
    auto candidate = ring->candidate(0, false);
    check(candidate && ring->protect(*candidate, reader.reader), "fixture pins reader token");
    const auto next = take(ring->reserve(closed));
    ok(ring->publish(next, detail::minimum_message_bytes, 2, 2, 2));

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
    const auto notification = receive_single_change(*manager, healthy_observer, healthy_observer_id);
    check(notification.operation == detail::Detach, "other recipients still receive Detach");
    auto failed_send = failed_observer.central->send(
        {control::Kind::Event, detail::message_bus_domain, detail::Detach});
    check(!failed_send, "failed notification closes its recipient");

    // Exact budgets and one Publisher per topic make leaked registry entries or
    // accounting observable. Retained test mappings do not count as active Bus budget.
    const auto replacement = register_publisher(*manager, writer, writer_id, "a-first");
    check(replacement.id != first.id, "publisher capacity and shared budget must be reusable");
    // Drain the expected Attach from the healthy observer before its next request.
    check(receive_single_change(*manager, healthy_observer, healthy_observer_id).operation == detail::Attach,
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
thread_local std::function<void()> before_release_wait;
thread_local std::function<void(Deadline)> after_release_wait;

Result<std::uint64_t> publication_timestamp() noexcept {
    return clock_override ? *clock_override : vinc::unix_nanoseconds();
}

Result<void> notification_notify(osal::NotificationSender& sender, std::uint64_t owner) {
    if (before_notification) {
        before_notification();
    }
    ++notification_calls;
    if (failed_notification_owner.load() == owner) {
        return std::unexpected(Error{osal::ErrorCode::SystemError, osal::ErrorOperation::Notify});
    }
    return sender.notify();
}

bool reject_topology_admission() noexcept {
    return reject_page.exchange(false);
}

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

#ifdef __linux__
// Link-time faults affect only this executable. All unarmed calls go to libc.
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __wrap_ftruncate(int fd, off_t size) {
    if (ring_sizes_before_failure && --ring_sizes_before_failure == 0) {
        errno = ENOSPC;
        return -1;
    }
    if (native_counter_fault == CounterFault::Size || native_counter_fault == CounterFault::SizeAndUnlink) {
        errno = ENOSPC;
        return -1;
    }
    return __real_ftruncate(fd, size);
}

extern "C" int __real_shm_unlink(const char*);
extern "C" int __wrap_shm_unlink(const char* name) {
    if (!ring_unlink_denied.empty() && std::string_view(name).ends_with(ring_unlink_denied)) {
        errno = EACCES;
        return -1;
    }
    if (native_counter_fault == CounterFault::SizeAndUnlink) {
        errno = EACCES;
        return -1;
    }
    return __real_shm_unlink(name);
}

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset) {
    if (ring_maps_before_failure && --ring_maps_before_failure == 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (native_counter_fault == CounterFault::Map) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, prot, flags, fd, offset);
}
#endif

int main(int argc, char** argv) {
    const char* phase = "startup";
    try {
        if (argc == 2) {
            const std::string_view scenario = argv[1];
            phase = argv[1];
            if (scenario == "images") {
                test_image_validation();
                test_image_notification_route();
                test_retired_source_before_dispatch<true>(false);
                test_retired_source_before_dispatch<true>(true);
                test_confirmed_missing_source<true>(false);
                test_confirmed_missing_source<true>(true);
                return 0;
            }
            if (scenario == "topology-load") {
                vinc::message_bus::detail::testing::run_topology_load_tests();
                return 0;
            }
            constexpr std::size_t burst_topics = 80; // Exceeds the reliable channel's 64-frame queue.
            if (scenario == "topology-control") {
                test_paused_topology(true, 1);
                test_paused_topology(false, 1);
                return 0;
            }
            if (scenario == "topology-admission") {
                test_topology_admission_retry();
                return 0;
            }
            if (scenario == "topology-snapshot" || scenario == "topology-replace") {
                constexpr std::size_t multipage_topics = 160;
                test_paused_topology(true, multipage_topics, true, scenario == "topology-replace");
                vinc::message_bus::detail::testing::report_topology_storage();
                return 0;
            }
            if (scenario == "retired-source") {
                test_retired_source_before_dispatch(false);
                test_retired_source_before_dispatch(true);
                return 0;
            }
            if (scenario == "confirmed-source") {
                test_confirmed_missing_source(false);
                test_confirmed_missing_source(true);
                return 0;
            }
            if (scenario == "topology-attach" || scenario == "topology-detach") {
                test_paused_topology(scenario == "topology-attach", burst_topics);
                return 0;
            }
            if (scenario == "timely-request" || scenario == "expired-request") {
                test_request_after_release_ack(scenario == "expired-request");
                return 0;
            }
            if (scenario == "reservation-control") {
                test_publisher_reservation(0);
                return 0;
            }
            if (scenario == "reservation-rollback") {
                test_second_ring_reserve(false);
                test_second_ring_reserve(true);
                test_publisher_reservation(2, true);
#ifdef __linux__
                for (unsigned allocation : {1u, 2u}) {
                    for (bool deny_unlink : {false, true}) {
                        test_native_ring_rollback(false, allocation, deny_unlink);
                        test_native_ring_rollback(true, allocation, deny_unlink);
                    }
                }
#endif
                return 0;
            }
            if (scenario == "reservation-ring-control" || scenario == "reservation-ring-payload") {
                test_publisher_reservation(scenario == "reservation-ring-control" ? 1 : 2);
                return 0;
            }
        }
        if (argc == 2 && std::string_view(argv[1]) == "counter-registration") {
            phase = "counter registration rollback";
            test_counter_registration(CounterFault::None);
            test_counter_registration(CounterFault::Reserve);
            test_counter_registration(CounterFault::None, true);
            test_counter_registration(CounterFault::Retire, true);
            test_counter_registration(CounterFault::RetireAllocation, true);
#ifdef __linux__
            test_counter_registration(CounterFault::Size);
            test_counter_registration(CounterFault::SizeAndUnlink);
            test_counter_registration(CounterFault::Map);
#endif
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "notification-snapshots") {
            phase = "notification snapshot contracts";
            vinc::message_bus::detail::testing::run_notification_snapshot_tests();
            test_notification_install_catchup();
            std::cout << "PASS: notification snapshot contracts\n";
            return 0;
        }
        phase = "test_publication_notification_failure()";
        test_publication_notification_failure();
        phase = "test_publication_times()";
        test_publication_times();
        phase = "test_topic_discovery()";
        test_topic_discovery();
        phase = "test_publisher_defaults()";
        test_publisher_defaults();
        phase = "test_topic_limit_read_failure()";
        test_topic_limit_read_failure();
        phase = "test_malformed_discovery()";
        test_malformed_discovery();
        phase = "test_client_topic_separators()";
        test_client_topic_separators();
        phase = "test_manager_topic_separators()";
        test_manager_topic_separators();
        phase = "test_readonly_timeout()";
        test_readonly_timeout();
        phase = "test_missing_release_ack()";
        test_missing_release_ack();
        phase = "test_submitted_mutation_timeout()";
        test_submitted_mutation_timeout();
        phase = "test_send_failure()";
        test_send_failure();
        phase = "test_advertise_failure(FaultPoint::ResponseCopy)";
        test_advertise_failure(FaultPoint::ResponseCopy);
        phase = "test_advertise_failure(FaultPoint::DescriptorDecode)";
        test_advertise_failure(FaultPoint::DescriptorDecode);
        phase = "test_advertise_failure(FaultPoint::PublisherAttach)";
        test_advertise_failure(FaultPoint::PublisherAttach);
        phase = "test_subscribe_failure(FaultPoint::ResponseCopy)";
        test_subscribe_failure(FaultPoint::ResponseCopy);
        phase = "test_subscribe_failure(FaultPoint::AttachmentDecode)";
        test_subscribe_failure(FaultPoint::AttachmentDecode);
        phase = "test_malformed_advertise(MalformedReply::Truncated)";
        test_malformed_advertise(MalformedReply::Truncated);
        phase = "test_malformed_advertise(MalformedReply::Schema)";
        test_malformed_advertise(MalformedReply::Schema);
        phase = "test_malformed_advertise(MalformedReply::ZeroId)";
        test_malformed_advertise(MalformedReply::ZeroId);
        phase = "test_malformed_advertise(MalformedReply::Dimensions)";
        test_malformed_advertise(MalformedReply::Dimensions);
        phase = "test_notification_capacity()";
        test_notification_capacity();
        phase = "test_notification_topology()";
        test_notification_topology();
        phase = "release confirmation";
        test_release_confirmation(false);
        test_release_confirmation(true);
        phase = "release backpressure";
        test_release_backpressure();
        phase = "release ordering";
        test_release_fence();
        phase = "batch ownership";
        test_batch_ownership();
        phase = "test_registration_capacity_reuse()";
        test_registration_capacity_reuse();
        phase = "test_exit_cleanup(FaultPoint::DetachEncode)";
        test_exit_cleanup(FaultPoint::DetachEncode);
        phase = "test_exit_cleanup(FaultPoint::DetachSend)";
        test_exit_cleanup(FaultPoint::DetachSend);
        phase = "test_exit_cleanup(FaultPoint::None)";
        test_exit_cleanup(FaultPoint::None);
        std::cout << "PASS: Bus transaction rollback and failure-safe exit cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << phase << ": " << error.what() << '\n';
        return 1;
    }
}
