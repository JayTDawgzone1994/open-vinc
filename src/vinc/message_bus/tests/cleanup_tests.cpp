#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <vinc/control/channel.hpp>
// Exercise actual Bus code and Socket sessions at allocation failure boundaries.
// Fault checkpoints throw only in this test executable; data-path tests use real mappings.
#include "bus_test_hooks.hpp"
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc/time.hpp>
#include <vinc/common_error.hpp>
#include <atomic>
#include <future>
#include <limits>
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
        ok(detail::RuntimeAccess::progress_releases(*client));
        auto frame = take(central->receive_until(deadline()));
        check(frame.kind == control::Kind::Request && frame.operation == detail::ReleaseBatch && frame.request,
              "release requires a correlated acknowledgement");
        control::Reader reader(frame.payload);
        check(reader.u32() == 1 && reader.u32() == operation && reader.u64() == id && reader.done(),
              "rollback must release exactly its registration");
        reply(frame, {});
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

    auto install = [&](bool enabled) {
        control::Frame frame{control::Kind::Event, detail::message_bus_domain, detail::NotificationRoutesChanged};
        control::put_u64(frame.payload, source.id);
        control::put_u32(frame.payload, enabled ? 1 : 0);
        if (enabled) {
            control::put_u64(frame.payload, 2);
            ok(control::put_string(frame.payload, name));
        }
        ok(detail::RuntimeAccess::receive(*connection.client, std::move(frame)));
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
    auto diagnostic = manager->last_counter_cleanup_failure();

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
        check(journal->reserved && diagnostic && diagnostic->resource_name == journal->name &&
              diagnostic->primary == result.error(), "lost recovery evidence or primary cause");
        if (fault == CounterFault::Retire) {
            check(journal->retire_calls == 1 && diagnostic->cleanup == journal->journal_error,
                  "journal removal failure lost or retried");
        } else if (fault == CounterFault::RetireAllocation) {
            check(journal->retire_calls == 1 && diagnostic->cleanup.is(bus::ErrorCode::ResourceExhausted),
                  "allocation during retirement hid the primary failure");
        } else {
#ifdef __linux__
            check(journal->retire_calls == 0 && diagnostic->cleanup.is_operation(osal::ErrorOperation::UnlinkSharedMemory) &&
                  diagnostic->cleanup.native.value() == EACCES && causes.back().native.value() == ENOSPC,
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

detail::Descriptor register_publisher(detail::BusManager& manager, Connection& connection,
                                      std::uint64_t owner, const std::string& topic, std::uint32_t expected_routes = 0) {
    control::Bytes payload;
    ok(control::put_string(payload, topic));
    control::put_u64(payload, Frame::schema().hash);
    control::put_u32(payload, detail::publisher_options_mask);
    control::put_u32(payload, fixture_slots);
    control::put_u64(payload, message_bytes);
    auto body = take(manager_request(manager, connection, owner, detail::Advertise, std::move(payload)));
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
    ok(detail::RuntimeAccess::progress_releases(*connection.client));
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
        const auto event = take(writer.module->receive_until(deadline()));
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
    route_event(publication.id, 1);
    const auto get_last_only = subscribe("routes");
    mutate(detail::Unsubscribe, first);
    route_event(publication.id, 1);
    mutate(detail::Unsubscribe, second);
    route_event(publication.id, 0);
    mutate(detail::Unsubscribe, get_last_only);
    const auto early = subscribe("sub-first");
    mutate(detail::EnableNotifications, early);
    const auto early_publication = register_publisher(*manager, writer, 1, "sub-first", 1);
    auto early_ring = take(detail::RingBuffer::open(early_publication, false));
    detail::shared_store(early_ring->reader(0).token, 1);
    check(take(reader.module->receive_until(deadline())).operation == detail::Attach,
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
    ok(detail::RuntimeAccess::progress_releases(*connection.client));
    check(closing.wait_for(0s) != std::future_status::ready, "a write to the socket is not completion");

    if (break_session) {
        detail::RuntimeAccess::shutdown(*connection.client);
        const auto result = closing.get();
        check(!result && result.error().is(bus::ErrorCode::Closed), "disconnect wakes unconfirmed close");
        return;
    }

    connection.reply(first, {});
    ok(detail::RuntimeAccess::progress_releases(*connection.client));
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

    ok(detail::RuntimeAccess::progress_releases(*connection.client));
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
    ok(detail::RuntimeAccess::progress_releases(*connection.client));
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
thread_local std::function<void()> before_release_wait;

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
    if (native_counter_fault == CounterFault::Size || native_counter_fault == CounterFault::SizeAndUnlink) {
        errno = ENOSPC;
        return -1;
    }
    return __real_ftruncate(fd, size);
}

extern "C" int __real_shm_unlink(const char*);
extern "C" int __wrap_shm_unlink(const char* name) {
    if (native_counter_fault == CounterFault::SizeAndUnlink) {
        errno = EACCES;
        return -1;
    }
    return __real_shm_unlink(name);
}

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset) {
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
