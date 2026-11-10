#include "../../control_channel/tests/channel_test_support.hpp"
#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/instance.hpp>
#include <vinc/rpc/detail/host.hpp>
#include <vinc/rpc/detail/storage.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc_messages/Frame.hpp>
#include <future>
#include "async_support.hpp"
#include <iostream>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
using channel_test::check;
using channel_test::take;
namespace rd = vinc::rpc::detail;
constexpr auto limit = 5s;
constexpr auto short_wait = 50ms;
constexpr std::uint64_t slot_capacity = 4096;

void require(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

// Own the real control worker and both sessions. Tests deliberately decide when
// Grant/Reply reach Host, without sleeping to guess a protocol ordering.
struct Transport {
    std::unique_ptr<control::Service> service = take(control::Service::create(2));
    std::shared_ptr<control::Session> local;
    std::shared_ptr<control::Session> remote;
    std::unique_ptr<rd::Host> host;
    std::shared_ptr<rd::CallBudget> budget = std::make_shared<rd::CallBudget>();
    Transport() {
        auto sockets = channel_test::connected_sockets();
        auto options = channel_test::options();
        local = take(service->attach(std::move(sockets.first), options));
        options.role = control::Role::Module;
        remote = take(service->attach(std::move(sockets.second), options));
        require(local->wait_open(control::Clock::now() + limit));
        require(remote->wait_open(control::Clock::now() + limit));
        host = std::make_unique<rd::Host>(local);
    }
    std::unique_ptr<rd::BlockingCall> start() {
        auto id = take(host->reserve_request());
        auto reservation = take(rd::CallBudget::reserve(budget, slot_capacity));
        auto call = take(host->start_call(rd::frame(rd::Acquire, id), std::move(reservation)));
        auto acquire = take(remote->receive_until(control::Clock::now() + limit));
        check(acquire.operation == rd::Acquire && acquire.request == id, "Acquire accepted exactly once");
        return call;
    }
    void event(rd::Operation operation, std::uint64_t id, control::Bytes bytes) {
        auto frame = rd::frame(operation, id, std::move(bytes));
        frame.kind = control::Kind::Response;
        require(host->receive(std::move(frame)));
    }
    std::uint64_t pending() {
        rpc::Stats stats;
        budget->inspect(stats);
        return stats.pending;
    }
    void released(std::uint64_t token) {
        require(host->progress());
        auto release = take(remote->receive_until(control::Clock::now() + limit));
        check(release.operation == rd::Release && release.request == token, "Release carries exact lease");
        check(pending() == 0 && !host->ready(), "retired without application pump or input event");
    }
};

control::Bytes success(control::Bytes body) {
    control::Bytes bytes;
    control::put_u32(bytes, rd::result_success);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

void late_events() {
    auto instance = take(vinc::detail::Instance::create(test::instance_name()));
    rpc::ServiceOptions options{slot_capacity, slot_capacity, 1};
    auto pool = take(rd::Pool::create(instance, options));
    rd::Transfer transfer{1, 1, pool->name(), pool->extent(), 0, slot_capacity, slot_capacity, 0};
    auto message = vinc::make_message<Frame>();
    auto image = take(message->snapshot());
    Transport transport;
    auto call = transport.start();
    auto expired = call->wait(image.bytes(), control::Clock::now());
    check(!expired && expired.error().is(rpc::ErrorCode::TimeoutOutcomeUnknown), "timeout before Grant");
    call.reset();
    require(transport.host->progress());
    check(transport.pending() == 1, "unknown Grant still charges budget");
    transport.event(rd::Grant, 1, success(rd::encode_transfer(transfer)));
    transport.event(rd::Grant, 1, success(rd::encode_transfer(transfer))); // Duplicate is not a second lease.
    transport.released(transfer.token);

    call = transport.start();
    transfer.token = 2;
    transport.event(rd::Grant, 2, success(rd::encode_transfer(transfer)));
    auto waiting = std::async(std::launch::async, [&] {
        return call->wait(image.bytes(), control::Clock::now() + short_wait);
    });
    auto submit = take(transport.remote->receive_until(control::Clock::now() + limit));
    check(submit.operation == rd::Submit && submit.request == transfer.token, "Submit precedes timeout");
    auto timeout = waiting.get();
    check(!timeout && timeout.error().is(rpc::ErrorCode::TimeoutOutcomeUnknown), "timeout after Submit");
    call.reset();
    require(transport.host->progress());
    check(transport.pending() == 1, "live target lease cannot be stolen on timeout");
    transport.event(rd::Reply, 2, rd::encode_failure(Error{rpc::ErrorCode::TargetExited, rpc::ErrorOperation::Call}));
    transport.released(transfer.token);

    call = transport.start();
    transport.event(rd::Grant, 3, rd::encode_failure(Error{rpc::ErrorCode::QueueFull, rpc::ErrorOperation::Call}));
    auto full = call->wait(image.bytes(), control::Clock::now() + limit);
    check(!full && full.error().is(rpc::ErrorCode::QueueFull), "remote admission error preserved");
    call.reset();
    require(transport.host->progress());
    check(transport.pending() == 0, "failed Grant requires no Release");
}

void stop_wakes_waiter() {
    Transport transport;
    auto call = transport.start();
    auto waiting = std::async(std::launch::async, [&] {
        return call->wait({}, control::Clock::now() + limit);
    });
    transport.host->stop();
    check(waiting.wait_for(1s) == std::future_status::ready, "stop wakes instead of waiting for call deadline");
    auto result = waiting.get();
    check(!result && result.error().is(rpc::ErrorCode::Closed), "stop returns Closed");
    call.reset();
    require(transport.host->progress()); // Stopped cleanup cannot become a new failure.
}

void closed_channel_rejects_admission() {
    Transport transport;
    transport.local->close();
    const auto id = take(transport.host->reserve_request());
    auto reservation = take(rd::CallBudget::reserve(transport.budget, slot_capacity));
    auto call = transport.host->start_call(rd::frame(rd::Acquire, id), std::move(reservation));
    check(!call && call.error().is(rpc::ErrorCode::ChannelFailed),
          "closed control channel rejects synchronous admission");
    check(transport.pending() == 0, "failed send returns local admission capacity");
}

void invalid_reply_preserves_response() {
    auto instance = take(vinc::detail::Instance::create(test::instance_name()));
    auto pool = take(rd::Pool::create(instance, {slot_capacity, slot_capacity, 1}));
    rd::Transfer transfer{1, 1, pool->name(), pool->extent(), 0, slot_capacity, slot_capacity, 0};
    Transport transport;
    rpc::Client client;
    rd::ClientAccess::bind(client, *transport.host);
    auto messages = make_rpc_messages<Frame, Frame>();
    messages.response().data().sequence = 123;
    auto waiting = std::async(std::launch::async, [&] {
        return client.call_sync({"server", "echo"}, messages, {limit});
    });
    auto acquire = take(transport.remote->receive_until(control::Clock::now() + limit));
    transport.event(rd::Grant, acquire.request, success(rd::encode_transfer(transfer)));
    auto submit = take(transport.remote->receive_until(control::Clock::now() + limit));
    check(submit.operation == rd::Submit, "real client transferred request");
    auto memory = take(rd::TransferMemory::open(transfer));
    const control::Bytes corrupt{std::byte{0}};
    require(memory.write(true, corrupt));
    control::Bytes reply;
    control::put_u32(reply, 0);
    require(control::put_string(reply, ""));
    control::put_u64(reply, corrupt.size());
    transport.event(rd::Reply, acquire.request, success(std::move(reply)));
    auto result = waiting.get();
    check(!result && result.error().is(rpc::ErrorCode::StorageFailed), "invalid layout returns StorageFailed");
    check(messages.response().data().sequence.get() == 123, "old response unchanged after decode failure");
    require(transport.host->progress());
    auto release = take(transport.remote->receive_until(control::Clock::now() + limit));
    check(release.operation == rd::Release && client.stats().pending == 0, "malformed response still releases lease");
}

void shared_admission_budget() {
    rpc_test::Clients clients;
    Transport transport;
    rpc::Client client;
    rd::ClientAccess::bind(client, *transport.host);
    auto messages = make_rpc_messages<Frame, Frame>();
    std::vector<std::uint64_t> ids;
    std::uint64_t delivered = 0;
    for (std::uint64_t index = 0; index < config::rpc_pending_limit; ++index) {
        require((clients.send<Frame, Frame>(client, "server", "echo", messages.request(), {},
            [&](Result<rpc::RpcReply<Frame>> reply) {
                check(!reply && reply.error().is(rpc::ErrorCode::NotFound), "async routing preserved");
                ++delivered;
            })));
        ids.push_back(take(transport.remote->receive_until(control::Clock::now() + limit)).request);
    }
    auto blocked = client.call_sync({"server", "echo"}, messages, {limit});
    check(!blocked && blocked.error().is(rpc::ErrorCode::QueueFull), "async occupancy blocks sync admission");
    for (auto id : ids) {
        transport.event(rd::Grant, id, rd::encode_failure(Error{rpc::ErrorCode::NotFound, rpc::ErrorOperation::Call}));
    }
    auto pumped = take(client.pump());
    check(delivered == config::rpc_pending_limit && pumped.completions == delivered,
          "only async completions belong to pump batch");
    check(client.stats().pending == 0, "shared capacity restored after async completion");
}

void budget_is_bounded() {
    auto budget = std::make_shared<rd::CallBudget>();
    std::vector<std::shared_ptr<rd::CallReservation>> held;
    for (std::uint64_t index = 0; index < config::rpc_pending_limit; ++index) {
        held.push_back(take(rd::CallBudget::reserve(budget, 1)));
    }
    check(!rd::CallBudget::reserve(budget, 1), "bounded pending budget");
    held.clear();
    auto full = take(rd::CallBudget::reserve(budget, config::rpc_local_bytes));
    check(!rd::CallBudget::reserve(budget, 1), "bounded image budget");
    full.reset();
    rpc::Stats stats;
    budget->inspect(stats);
    check(stats.pending == 0 && stats.request_bytes == 0, "all reservations returned");
}
}

int main() {
    try {
        late_events();
        stop_wakes_waiter();
        closed_channel_rejects_admission();
        invalid_reply_preserves_response();
        budget_is_bounded();
        shared_admission_budget();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
