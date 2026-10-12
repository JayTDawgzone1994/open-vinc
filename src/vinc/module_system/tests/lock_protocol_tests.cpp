#include "../../control_channel/tests/channel_test_support.hpp"
#include <vinc/module/detail/module_locks.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <future>
#include <iostream>

namespace {
using namespace vinc;
using channel_test::check;
using channel_test::take;
constexpr MID fixture_mid = 7;
constexpr std::uint64_t resource_id = 1;
constexpr auto timeout = std::chrono::seconds(5);
constexpr auto short_timeout = std::chrono::milliseconds(30);
Deadline deadline() {
    return control::Clock::now() + timeout;
}

control::Bytes payload(std::uint32_t version, std::uint64_t id, MID mid) {
    control::Bytes bytes;
    control::put_u32(bytes, detail::result_success);
    control::put_u32(bytes, version);
    control::put_u64(bytes, id);
    control::put_u64(bytes, mid);
    check(bool(control::put_string(bytes, "nonexistent")), "encode name");
    return bytes;
}

void run() {
    auto central_service = take(control::Service::create(1));
    auto module_service = take(control::Service::create(1));
    auto sockets = channel_test::connected_sockets();
    auto options = channel_test::options();
    auto central = take(central_service->attach(std::move(sockets.first), options));
    options.role = control::Role::Module;
    auto module = take(module_service->attach(std::move(sockets.second), options));
    check(bool(central->wait_open(deadline())), "central handshake");
    check(bool(module->wait_open(deadline())), "module handshake");
    detail::ModuleLocks locks(module, fixture_mid);
    const auto exchange = [&](control::Bytes bytes) {
        auto request = std::async(std::launch::async, [&] {
            return locks.lock(resource_id, deadline());
        });
        const auto frame = take(central->receive_until(deadline()));
        check(frame.operation == detail::attach_lock, "attach operation");
        control::Frame reply{control::Kind::Response, detail::lifecycle, detail::attach_lock, frame.request};
        reply.payload = std::move(bytes);
        check(bool(central->send(std::move(reply))), "reply send");
        check(bool(locks.receive(take(module->receive_until(deadline())))), "watchdog dispatch");
        return request.get();
    };

    const auto protocol_failure = [&](control::Bytes bytes) {
        auto result = exchange(std::move(bytes));
        check(!result && result.error().is(module_system::ErrorCode::Protocol), "malformed reply accepted");
    };
    protocol_failure(payload(detail::lock_protocol_version + 1, resource_id, fixture_mid));
    protocol_failure(payload(detail::lock_protocol_version, resource_id + 1, fixture_mid));
    protocol_failure(payload(detail::lock_protocol_version, resource_id, fixture_mid + 1));
    auto truncated = payload(detail::lock_protocol_version, resource_id, fixture_mid);
    truncated.pop_back();
    protocol_failure(std::move(truncated));
    auto trailing = payload(detail::lock_protocol_version, resource_id, fixture_mid);
    trailing.push_back(std::byte{0});
    protocol_failure(std::move(trailing));

    const auto cause = Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory};
    const auto failure = wrap_error(module_system::ErrorCode::LockFailed,
                                    module_system::ErrorOperation::AccessLock, cause);
    auto rejected = exchange(detail::encode_failure(failure));
    check(!rejected && rejected.error().is(module_system::ErrorCode::LockFailed) &&
              rejected.error().causes().size() == 1 &&
              rejected.error().causes().front().domain == ErrorDomain::Osal,
          "central error cause lost");

    auto expired = locks.lock(resource_id, control::Clock::now() + short_timeout);
    check(!expired && expired.error().is(module_system::ErrorCode::Timeout), "request timeout");
    const auto late = take(central->receive_until(deadline()));
    control::Frame late_reply{control::Kind::Response, detail::lifecycle, detail::attach_lock, late.request};
    check(bool(locks.receive(std::move(late_reply))), "late timed-out response should be retired");
    control::Frame unknown{control::Kind::Response, detail::lifecycle, detail::attach_lock, UINT64_MAX};
    auto invalid = locks.receive(std::move(unknown));
    check(!invalid && invalid.error().is(module_system::ErrorCode::Protocol), "unknown request accepted");

    auto waiting = std::async(std::launch::async, [&] {
        return locks.lock(resource_id, (Deadline::max)());
    });
    take(central->receive_until(deadline()));
    locks.stop();
    check(waiting.wait_for(timeout) == std::future_status::ready, "pending lookup did not cancel");
    auto cancelled = waiting.get();
    check(!cancelled && cancelled.error().is(module_system::ErrorCode::Cancelled), "lookup cancellation code");
}
}

int main() {
    try {
        run();
        std::cout << "PASS: lock grant protocol, cause chain, timeout, cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
