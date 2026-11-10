#include <vinc/rpc/detail/rpc_client_access.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc/rpc/detail/error_protocol.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc_messages/Empty.hpp>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using Client = rpc::RpcClient<Frame, Frame>;
using Messages = rpc::RpcMessages<Frame, Empty>;

static_assert(!rpc::Message<int>);
static_assert(rpc::Message<Frame>);
static_assert(!std::is_copy_constructible_v<Client>);
static_assert(std::is_move_constructible_v<Client>);
static_assert(std::is_move_assignable_v<Client>);
static_assert(std::is_copy_constructible_v<Messages>);
static_assert(std::is_move_constructible_v<Messages>);
static_assert(!std::is_copy_assignable_v<Messages>);
static_assert(!std::is_move_assignable_v<Messages>);
static_assert(std::is_same_v<decltype(std::declval<const Messages&>().request()), const Frame&>);
static_assert(std::is_same_v<decltype(std::declval<Messages&>().response()), Empty&>);

void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

template <class T>
auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

template <class T>
void expect_error(const Result<T>& result, rpc::ErrorCode code, const char* description) {
    check(!result && result.error().is(code), description);
}

// A controlled submission boundary, not a Host simulation. Completion is
// delivered explicitly by the test so no hidden pump/thread can pass a test.
struct Endpoint final : rpc::detail::CallEndpoint {
    struct Job {
        rpc::detail::Address address;
        message::Snapshot image;
        rpc::CallOptions options;
        rpc::detail::Completion completion;
    };

    std::vector<Job> jobs;
    std::optional<Error> rejection;
    bool throw_on_submit = false;

    Result<void> submit(const rpc::detail::Address& address, std::uint64_t request_hash,
                        std::uint64_t response_hash, message::Snapshot image,
                        rpc::CallOptions options, rpc::detail::Completion completion) override {
        check(request_hash == Frame::schema().hash, "request schema forwarded");
        check(response_hash == Frame::schema().hash, "response schema forwarded");
        if (throw_on_submit) {
            throw std::bad_alloc();
        }

        if (rejection) {
            return std::unexpected(*rejection);
        }

        jobs.push_back({address, std::move(image), options, std::move(completion)});
        return {};
    }
};

rpc::detail::RawResult reply(std::uint64_t sequence, rpc::RpcStatus status = {}) {
    auto message = make_message<Frame>();
    message->data().sequence = sequence;
    auto image = take(message->snapshot());
    return {std::move(status), control::Bytes(image.bytes().begin(), image.bytes().end())};
}

void address_and_creation() {
    auto parsed = take(rpc::detail::parse_address("camera-1.get_count"));
    check(parsed.module == "camera-1" && parsed.service == "get_count", "address segments");
    for (const auto address : {"", "receiver", ".count", "receiver.", "receiver..count",
                               "receiver.count.extra", "receiver/count", "receiver.c/ount",
                               " receiver.count", "receiver.co unt", "receiver.c\\ount"}) {
        auto invalid = rpc::detail::parse_address(address);
        expect_error(invalid, rpc::ErrorCode::InvalidName, "invalid address rejected");
        check(invalid.error().is_operation(rpc::ErrorOperation::Create), "address error operation");
    }

    const std::string embedded_nul("receiver.co\0unt", 15);
    expect_error(rpc::detail::parse_address(embedded_nul), rpc::ErrorCode::InvalidName, "NUL rejected");
    const std::string non_ascii = "receiver." + std::string(1, static_cast<char>(0xff));
    expect_error(rpc::detail::parse_address(non_ascii), rpc::ErrorCode::InvalidName, "non-ASCII rejected");
    const std::string maximum(config::rpc_name_bytes, 'a');
    check(rpc::detail::parse_address(maximum + "." + maximum).has_value(), "per-segment maximum");
    expect_error(rpc::detail::parse_address(maximum + "a.count"), rpc::ErrorCode::InvalidName,
                 "module length enforced");
    expect_error(rpc::detail::parse_address("receiver." + maximum + "a"), rpc::ErrorCode::InvalidName,
                 "service length enforced");

    auto endpoint = std::make_shared<Endpoint>();
    auto callback = [](auto) {};
    expect_error(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "bad/address", callback),
                 rpc::ErrorCode::InvalidName, "factory validates address before submission");
    expect_error(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "a.b", callback,
                                                                  {std::chrono::milliseconds::zero()}),
                 rpc::ErrorCode::InvalidOptions, "zero timeout rejected");
    expect_error(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "a.b", callback,
                         {config::rpc_max_call_timeout + std::chrono::milliseconds(1)}),
                 rpc::ErrorCode::InvalidOptions, "timeout above limit rejected");
    std::function<void(Result<rpc::RpcReply<Frame>>)> empty;
    expect_error(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "a.b", empty),
                 rpc::ErrorCode::InvalidOptions, "empty callback rejected");
    expect_error(rpc::detail::RpcClientAccess::create<Frame, Frame>({}, "a.b", callback),
                 rpc::ErrorCode::Closed, "absent Context endpoint rejected");
    check(endpoint->jobs.empty(), "creation performs no remote operation");
}

void message_ownership() {
    auto messages = make_rpc_messages<Frame, Frame>();
    messages.request().data().sequence = 7;
    messages.response().data().sequence = 19;
    auto copy = messages;
    copy.request().data().sequence = 8;
    copy.response().data().sequence = 20;
    check(messages.request().data().sequence.get() == 7, "request copy is independent");
    check(messages.response().data().sequence.get() == 19, "response copy is independent");
    auto moved = std::move(copy);
    check(moved.request().data().sequence.get() == 8, "request survives ownership move");
    check(moved.response().data().sequence.get() == 20, "response survives ownership move");
    take(moved.response().assign(messages.response()));
    check(moved.response().data().sequence.get() == 19, "normal message assign remains available");
}

void repeated_calls_and_snapshots() {
    auto endpoint = std::make_shared<Endpoint>();
    std::vector<std::uint64_t> received;
    std::unique_ptr<Frame> retained;
    std::int32_t application_code = 0;
    const auto pump_thread = std::this_thread::get_id();
    const rpc::CallOptions defaults{std::chrono::milliseconds(450)};
    const rpc::CallOptions override{std::chrono::milliseconds(900)};
    std::string address = "receiver.get_count";
    auto client = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(
        endpoint, address,
        [&](auto result) {
            check(std::this_thread::get_id() == pump_thread, "callback stays on dispatch thread");
            auto value = take(std::move(result));
            application_code = value.status.code;
            received.push_back(value.response->data().sequence.get());
            retained = std::move(value.response);
        }, defaults));
    address.assign("changed.other");
    auto request = client.make_request();
    request->data().sequence = 31;
    take(client.call(*request));
    request->data().sequence = 32;
    check(received.empty(), "admission does not dispatch callback");
    check(endpoint->jobs.front().address.module == "receiver", "address ownership");
    check(endpoint->jobs.front().address.service == "get_count", "service ownership");
    auto copied = take(Frame::from_bytes(endpoint->jobs.front().image.bytes()));
    check(copied->data().sequence.get() == 31, "request snapshot independent of later mutation");
    expect_error(client.call(*request), rpc::ErrorCode::Busy, "one pending call per client");
    check(endpoint->jobs.size() == 1, "Busy did not submit");

    auto first = endpoint->jobs.front().completion;
    first(reply(41, rpc::RpcStatus::failure(9, "application rejection")));
    check(application_code == 9 && retained->data().sequence.get() == 41, "application failure keeps response");
    take(client.call(*request, override));
    first(reply(99));
    check(received.size() == 1, "duplicate completion ignored");
    expect_error(client.call(*request), rpc::ErrorCode::Busy, "old completion cannot clear new busy state");
    auto second = endpoint->jobs.back().completion;
    second(reply(42));
    take(client.call(*request));
    check(endpoint->jobs[0].options.timeout == defaults.timeout, "client default timeout");
    check(endpoint->jobs[1].options.timeout == override.timeout, "single-call override");
    check(endpoint->jobs[2].options.timeout == defaults.timeout, "override does not change default");
    client.close();
    endpoint->jobs.back().completion(reply(100));
    check(received == std::vector<std::uint64_t>{41, 42}, "close suppresses future dispatch");
    check(retained->data().sequence.get() == 42, "response outlives client close");
    expect_error(client.call(*request), rpc::ErrorCode::Closed, "closed handle rejects calls");
}

void failure_and_retry() {
    auto endpoint = std::make_shared<Endpoint>();
    std::vector<Error> failures;
    auto client = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(
        endpoint, "receiver.get_count", [&](auto result) {
            check(!result, "expected failed reply");
            failures.push_back(result.error());
        }));
    auto request = client.make_request();
    endpoint->rejection = Error{rpc::ErrorCode::QueueFull, rpc::ErrorOperation::Call};
    expect_error(client.call(*request), rpc::ErrorCode::QueueFull, "immediate admission failure");
    check(failures.empty(), "immediate failure has no callback");
    endpoint->rejection.reset();
    endpoint->throw_on_submit = true;
    bool allocation_exception = false;
    try {
        (void)client.call(*request);
    } catch (const std::bad_alloc&) {
        allocation_exception = true;
    }
    check(allocation_exception, "existing allocation exception policy retained");
    endpoint->throw_on_submit = false;
    take(client.call(*request));
    auto first = endpoint->jobs.back().completion;
    const auto cause = Error::from_raw(static_cast<ErrorDomain>(777), -123, 432);
    const auto failure = wrap_error(rpc::ErrorCode::ChannelFailed, rpc::ErrorOperation::Call, cause);
    first(std::unexpected(failure));
    check(failures.back() == failure, "complete framework error chain preserved");
    take(client.call(*request));
    endpoint->jobs.back().completion(rpc::detail::RawResult{});
    check(failures.back().is(rpc::ErrorCode::StorageFailed), "malformed response is a storage failure");
    take(client.call(*request));
    auto wrong_type = make_message<Empty>();
    auto wrong_image = take(wrong_type->snapshot());
    endpoint->jobs.back().completion(rpc::detail::RawResult{
        {}, control::Bytes(wrong_image.bytes().begin(), wrong_image.bytes().end())});
    check(failures.back().is(rpc::ErrorCode::StorageFailed), "wrong response schema rejected");
    expect_error(client.call(*request, {std::chrono::milliseconds(-1)}), rpc::ErrorCode::InvalidOptions,
                 "invalid per-call timeout rejected");
    take(client.call(*request));
    auto delayed = endpoint->jobs.back().completion;
    endpoint.reset();
    delayed(std::unexpected(Error{rpc::ErrorCode::Closed, rpc::ErrorOperation::Call}));
    expect_error(client.call(*request), rpc::ErrorCode::Closed, "client does not retain Context endpoint");

    auto encoded = rpc::detail::encode_failure(Error{rpc::ErrorCode::Busy, rpc::ErrorOperation::Call});
    auto decoded = take(rpc::detail::decode_response(encoded));
    check(decoded.failure && decoded.failure->is(rpc::ErrorCode::Busy), "Busy error protocol round trip");
    check(error_to_string(*decoded.failure).find("outstanding call") != std::string::npos,
          "Busy error has static catalog text");
}

void moves_destruction_and_resubmission() {
    auto endpoint = std::make_shared<Endpoint>();
    Client client;
    auto request = client.make_request();
    expect_error(client.call(*request), rpc::ErrorCode::Closed, "empty handle has no authority");
    std::size_t callbacks = 0;
    client = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "receiver.get_count",
        [&](auto result) {
            take(std::move(result));
            ++callbacks;
            if (callbacks == 1) {
                take(client.call(*request));
            }
        }));
    take(client.call(*request));
    auto first = endpoint->jobs.back().completion;
    first(reply(1));
    check(endpoint->jobs.size() == 2, "callback can resubmit without Busy");
    auto moved = std::move(client);
    expect_error(client.call(*request), rpc::ErrorCode::Closed, "moved-from handle closed");
    endpoint->jobs.back().completion(reply(2));
    check(callbacks == 2, "move preserves pending callback");

    std::weak_ptr<int> captured;
    rpc::detail::Completion after_destruction;
    {
        auto owned = std::make_shared<int>(1);
        captured = owned;
        auto scoped = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "other.echo",
            [owned](auto) {
                throw std::runtime_error("destroyed client's callback ran");
            }));
        take(scoped.call(*request));
        after_destruction = endpoint->jobs.back().completion;
    }
    check(captured.expired(), "pending transport does not retain application callback captures");
    after_destruction(reply(3));

    take(moved.call(*request));
    auto replaced_completion = endpoint->jobs.back().completion;
    moved = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(endpoint, "new.echo", [](auto) {}));
    replaced_completion(reply(4));
    check(callbacks == 2, "move assignment retires previous destination callback");
    take(moved.call(*request));
}

void context_endpoint_revocation() {
    auto owner = std::make_unique<rpc::Client>();
    auto endpoint = rpc::detail::ClientAccess::endpoint(*owner).lock();
    auto retained = take(rpc::detail::RpcClientAccess::create<Frame, Frame>(
        endpoint, "receiver.echo", [](auto) {
            throw std::runtime_error("revoked endpoint dispatched a callback");
        }));
    // Keep the endpoint itself alive, as an in-flight admission would. Expiring
    // a weak pointer alone cannot prove that the raw owner pointer was revoked.
    owner.reset();
    auto request = retained.make_request();
    expect_error(retained.call(*request), rpc::ErrorCode::Closed,
                 "destroyed Context client revokes even a retained endpoint");
    endpoint.reset();
    expect_error(retained.call(*request), rpc::ErrorCode::Closed,
                 "handle remains safe after endpoint destruction");
}
}

int main() {
    try {
        address_and_creation();
        message_ownership();
        repeated_calls_and_snapshots();
        failure_and_retry();
        moves_destruction_and_resubmission();
        context_endpoint_revocation();
        std::cout << "RPC client foundations passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
