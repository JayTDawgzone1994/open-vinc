#include "channel_test_support.hpp"
#include "receive_wake_test_hooks.hpp"
#include <future>
#include <iostream>
#include <map>

namespace {
using namespace channel_test;
using Wake = control::detail::ReceiveWake;
using Access = control::detail::ReceiveWakeAccess;
constexpr auto gate_timeout = 5s;
constexpr int signal_count = 32;

enum class Boundary { None, Wait, Signal };
struct Gates {
    std::mutex mutex;
    std::condition_variable changed;
    const Wake* target = nullptr;
    Boundary boundary = Boundary::None;
    bool paused = false;
    bool released = false;
    bool expired = false;
    unsigned waits = 0;
    std::map<std::pair<std::uint64_t, control::Kind>, unsigned> received;
} gates;

// Declare after worker futures so unwinding releases gates BEFORE joining them.
// Gate waits themselves have a deadline, including failure before RAII setup.
struct Release {
    ~Release() {
        std::lock_guard lock(gates.mutex);
        gates.released = true;
        gates.target = nullptr;
        gates.changed.notify_all();
    }
};

void arm(const Wake* wake, Boundary boundary) {
    std::lock_guard lock(gates.mutex);
    gates.target = wake;
    gates.boundary = boundary;
    gates.paused = false;
    gates.released = false;
    gates.waits = 0;
}

void release() {
    std::lock_guard lock(gates.mutex);
    gates.released = true;
    gates.changed.notify_all();
}

template<class Predicate>
void await(Predicate predicate, const char* reason) {
    std::unique_lock lock(gates.mutex);
    check(gates.changed.wait_for(lock, gate_timeout, predicate), reason);
    check(!gates.expired, "test gate expired");
}

template<class T>
T finish(std::future<T>& future) {
    check(future.wait_for(gate_timeout) == std::future_status::ready, "test worker deadline");
    return future.get();
}

void check_timeout(Wake& wake) {
    const auto result = wake.wait_until(control::Clock::now());
    check(!result && result.error() == Error{control::ErrorCode::Timeout,
                                             control::ErrorOperation::WaitReceiveWake}, "wake timeout identity");
}

void test_pending_and_scan() {
    auto wake = std::make_shared<Wake>();
    check(!wake->consume(), "new wake starts empty");
    check_timeout(*wake);
    for (int index = 0; index < signal_count; ++index) {
        wake->notify();
    }
    check(bool(wake->wait_until(control::Clock::now())), "pre-wait hints survive expired deadline");
    check(wake->consume() && !wake->consume(), "many signals coalesce into one hint");

    // A notification during the receiver's scan must remain for the next wait.
    check(!wake->consume(), "begin scan empty");
    auto producer = std::async(std::launch::async, [wake] { wake->notify(); });
    finish(producer);
    check(bool(wake->wait_until(control::Clock::now())), "scan-time signal retained");
    check(wake->consume(), "wait does not consume hint");
    check_timeout(*wake);
}

void test_wait_transition_and_spurious() {
    auto wake = std::make_shared<Wake>();
    arm(wake.get(), Boundary::Wait);
    auto waiter = std::async(std::launch::async, [wake] {
        return wake->wait_until(control::Clock::now() + gate_timeout);
    });
    Release cleanup;
    await([] { return gates.paused; }, "waiter at atomic wait boundary");
    release();
    // Acquiring the wake mutex cannot finish until wait atomically releases it.
    // No sleep guesses whether the waiter has entered the OS wait yet.
    control::detail::ReceiveWakeTestAccess::spurious(*wake);
    await([] { return gates.waits >= 2; }, "spurious wake rechecks predicate");
    wake->notify();
    check(bool(finish(waiter)), "notification at wait boundary not lost");
    check(wake->consume(), "pending preserved after wait");
}

void test_stop_and_waiter_lifetime() {
    auto wake = std::make_shared<Wake>();
    std::weak_ptr<Wake> weak = wake;
    std::stop_source stop;
    arm(wake.get(), Boundary::Wait);
    auto waiter = std::async(std::launch::async, [owner = wake, token = stop.get_token()] {
        return owner->wait_until(control::Clock::now() + gate_timeout, token);
    });
    Release cleanup;
    await([] { return gates.paused; }, "cancellable waiter reached boundary");
    wake.reset();
    check(!weak.expired(), "waiter keeps its owner alive");
    release();
    stop.request_stop();
    const auto result = finish(waiter);
    check(!result && result.error() == Error{control::ErrorCode::Cancelled,
                                             control::ErrorOperation::WaitReceiveWake}, "cancel wakes blocked wait");
    check(weak.expired(), "owner released after waiter exits");

    Wake pending;
    pending.notify();
    const auto cancelled = pending.wait_until(control::Clock::now(), stop.get_token());
    check(!cancelled && cancelled.error().is(control::ErrorCode::Cancelled), "stop wins over hint");
    check(pending.consume(), "cancellation preserves pending work");
}

control::Options next_options() {
    static std::uint64_t generation = 100;
    auto value = options();
    value.generation = generation++;
    return value;
}

void await_received(std::uint64_t generation, control::Kind kind, unsigned count) {
    await([=] { return gates.received[{generation, kind}] >= count; }, "reactor accepted expected frames");
}

void send_hello(osal::Socket& remote, const control::Options& options) {
    control::Frame hello;
    hello.kind = control::Kind::Hello;
    control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Module));
    control::put_u64(hello.payload, options.generation);
    check(bool(control::put_string(hello.payload, options.token)), "hello token");
    control::put_u32(hello.payload, static_cast<std::uint32_t>(options.heartbeat_interval.count()));
    control::put_u32(hello.payload, static_cast<std::uint32_t>(options.heartbeat_timeout.count()));
    control::put_u32(hello.payload, control::hello_version);
    control::put_u32(hello.payload, options.heartbeat_timeout_enabled ? 1 : 0);
    send_frame(remote, hello);
}

void test_handshake_heartbeat_and_receive() {
    auto service = take(control::Service::create());
    const auto options = next_options();
    auto peer = attach_raw(*service, options, false);
    auto wake = std::make_shared<Wake>();
    Access::bind(*peer.session, wake);
    check(!wake->consume(), "handshaking empty Session has no ready hint");
    send_hello(peer.remote, options);
    check(bool(wake->wait_until(control::Clock::now() + operation_timeout)), "handshake wakes receiver");
    check(peer.session->state() == control::State::Open, "handshake state published before wake");
    check(wake->consume(), "consume Open transition");

    control::Frame heartbeat;
    heartbeat.kind = control::Kind::Heartbeat;
    control::put_u64(heartbeat.payload, 1);
    send_frame(peer.remote, heartbeat);
    await_received(options.generation, control::Kind::Heartbeat, 1);
    check(!wake->consume(), "valid heartbeat does not wake application receiver");

    send_frame(peer.remote, application());
    check(bool(wake->wait_until(control::Clock::now() + operation_timeout)), "application wakes receiver");
    check(wake->consume(), "application pending");
    const auto frame = take(peer.session->receive_until(control::Clock::now() + operation_timeout));
    check(frame.kind == control::Kind::Event, "wake did not consume frame; receive_until still works");
    check(!take(peer.session->try_receive()), "frame delivered exactly once");
}

void test_backlog_rebind_and_coalescing() {
    auto service = take(control::Service::create());
    const auto options = next_options();
    auto peer = attach_raw(*service, options);
    send_frame(peer.remote, application());
    await_received(options.generation, control::Kind::Event, 1);
    auto old = std::make_shared<Wake>();
    Access::bind(*peer.session, old);
    check(old->consume(), "binding observes existing backlog/Open state");
    check(take(peer.session->try_receive()).has_value(), "pre-bind frame preserved");
    Access::bind(*peer.session, {});
    send_frame(peer.remote, application());
    await_received(options.generation, control::Kind::Event, 2);
    check(!old->consume(), "detached owner receives no new hint");

    auto current = std::make_shared<Wake>();
    Access::bind(*peer.session, current);
    check(current->consume(), "new owner sees backlog without another frame");
    check(take(peer.session->try_receive()).has_value(), "rebound backlog preserved");
    for (int index = 0; index < signal_count; ++index) {
        auto frame = application();
        frame.operation = static_cast<std::uint16_t>(index + 1);
        send_frame(peer.remote, frame);
    }
    await_received(options.generation, control::Kind::Event, signal_count + 2);
    check(current->consume() && !current->consume(), "received burst coalesced");
    for (int index = 0; index < signal_count; ++index) {
        const auto frame = take(peer.session->try_receive());
        check(frame && frame->operation == index + 1, "shared inbox retains FIFO");
    }
    check(!old->consume(), "rebind never signals old owner");

    peer.session->close();
    check(bool(current->wait_until(control::Clock::now() + operation_timeout)), "close notifies owner");
    check(peer.session->state() == control::State::Closed, "terminal state visible");
    auto late = std::make_shared<Wake>();
    Access::bind(*peer.session, late);
    check(late->consume(), "binding after closure publishes terminal hint");
}

void test_inflight_lifetime_and_detach() {
    auto service = take(control::Service::create());
    const auto options = next_options();
    auto peer = attach_raw(*service, options);
    auto owner = std::make_shared<Wake>();
    Access::bind(*peer.session, owner);
    check(owner->consume(), "consume binding hint");
    std::weak_ptr<Wake> weak = owner;
    arm(owner.get(), Boundary::Signal);
    send_frame(peer.remote, application());
    // The signal owns a strong reference and Peer mutex at this checkpoint.
    await([] { return gates.paused; }, "signal acquired strong owner");
    auto detach = std::async(std::launch::async, [&] { Access::bind(*peer.session, {}); });
    Release cleanup;
    owner.reset();
    check(!weak.expired(), "inflight signal pins owner after external release");
    release();
    finish(detach);
    check(weak.expired(), "detach waits for old signal to finish");
    check(take(peer.session->receive_until(control::Clock::now() + operation_timeout)).kind == control::Kind::Event,
          "detaching does not discard frame");
    auto replacement = std::make_shared<Wake>();
    Access::bind(*peer.session, replacement);
    check(replacement->consume(), "replacement sees Open Session");
    send_frame(peer.remote, application());
    check(bool(replacement->wait_until(control::Clock::now() + operation_timeout)), "replacement gets new work");
}

void test_expired_owner_and_service_shutdown() {
    auto service = take(control::Service::create());
    const auto options = next_options();
    auto peer = attach_raw(*service, options);
    auto owner = std::make_shared<Wake>();
    std::weak_ptr<Wake> weak = owner;
    Access::bind(*peer.session, owner);
    owner.reset();
    check(weak.expired(), "Session does not retain notification owner");
    send_frame(peer.remote, application());
    await_received(options.generation, control::Kind::Event, 1);
    check(take(peer.session->try_receive()).has_value(), "expired owner does not affect receive");

    auto final_owner = std::make_shared<Wake>();
    Access::bind(*peer.session, final_owner);
    check(final_owner->consume(), "consume last binding hint");
    service.reset();
    check(bool(final_owner->wait_until(control::Clock::now())), "Service shutdown leaves terminal hint");
    check(peer.session->state() == control::State::Closed, "Session outlives Service safely");
}

void test_rebind_during_signal() {
    auto service = take(control::Service::create());
    const auto options = next_options();
    auto peer = attach_raw(*service, options);
    auto old = std::make_shared<Wake>();
    auto replacement = std::make_shared<Wake>();
    Access::bind(*peer.session, old);
    check(old->consume(), "consume old binding");
    arm(old.get(), Boundary::Signal);
    send_frame(peer.remote, application());
    await([] { return gates.paused; }, "old signal entered before rebind");
    auto rebind = std::async(std::launch::async, [&] { Access::bind(*peer.session, replacement); });
    Release cleanup;
    release();
    finish(rebind);
    check(old->consume(), "old in-flight hint may remain after rebind");
    check(replacement->consume(), "rebind catches the frame already queued under old owner");
    check(take(peer.session->try_receive()).has_value(), "rebind preserves queued frame");
    send_frame(peer.remote, application());
    await_received(options.generation, control::Kind::Event, 2);
    check(replacement->consume() && !old->consume(), "subsequent signal targets only new owner");
}

void test_shared_owner() {
    auto service = take(control::Service::create());
    const auto first_options = next_options();
    const auto second_options = next_options();
    auto first = attach_raw(*service, first_options);
    auto second = attach_raw(*service, second_options);
    auto wake = std::make_shared<Wake>();
    Access::bind(*first.session, wake);
    Access::bind(*second.session, wake);
    check(wake->consume() && !wake->consume(), "Open hints from two Sessions coalesce");
    send_frame(first.remote, application());
    send_frame(second.remote, application());
    await_received(first_options.generation, control::Kind::Event, 1);
    await_received(second_options.generation, control::Kind::Event, 1);
    check(wake->consume() && !wake->consume(), "one hint covers both Session inboxes");
    check(take(first.session->try_receive()).has_value(), "first inbox independent");
    check(take(second.session->try_receive()).has_value(), "second inbox independent");
    first.session->close();
    check(bool(wake->wait_until(control::Clock::now() + operation_timeout)), "shared owner sees one closure");
    check(first.session->state() == control::State::Closed &&
          second.session->state() == control::State::Open, "notification does not propagate closure");
}
}

namespace vinc::control::test {
namespace {
void checkpoint(const detail::ReceiveWake* wake, Boundary boundary) {
    std::unique_lock lock(gates.mutex);
    if (gates.target != wake) {
        return;
    }
    if (boundary == Boundary::Wait) {
        ++gates.waits;
        gates.changed.notify_all();
    }
    if (gates.boundary != boundary || gates.released) {
        return;
    }
    gates.paused = true;
    gates.changed.notify_all();
    if (!gates.changed.wait_for(lock, gate_timeout, [] { return gates.released; })) {
        gates.expired = true;
        gates.changed.notify_all();
    }
}
}
void before_receive_wait(const detail::ReceiveWake* wake) {
    checkpoint(wake, Boundary::Wait);
}
void before_receive_signal(const detail::ReceiveWake* wake) {
    checkpoint(wake, Boundary::Signal);
}
void received(std::uint64_t generation, Kind kind) {
    std::lock_guard lock(gates.mutex);
    ++gates.received[{generation, kind}];
    gates.changed.notify_all();
}
}

int main() {
    try {
        test_pending_and_scan();
        test_wait_transition_and_spurious();
        test_stop_and_waiter_lifetime();
        test_handshake_heartbeat_and_receive();
        test_backlog_rebind_and_coalescing();
        test_inflight_lifetime_and_detach();
        test_expired_owner_and_service_shutdown();
        test_rebind_during_signal();
        test_shared_owner();
        check(!gates.expired, "no bounded test gate expired");
        std::cout << "PASS: receive wake ordering, coalescing, cancellation and lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
