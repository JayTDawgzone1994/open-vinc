#include "../../control_channel/tests/channel_test_support.hpp"
#include <vinc/module/detail/module_queries.hpp>
#include <vinc/module/detail/module_query_protocol.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <future>
#include <iostream>

namespace {
using namespace vinc;
using channel_test::check;
using channel_test::take;
constexpr auto timeout = std::chrono::seconds{5};
constexpr auto short_timeout = std::chrono::milliseconds{30};
Deadline deadline() {
    return control::Clock::now() + timeout;
}
void notification_codec() {
    ModuleNotifications notifications;
    notifications.observed = true;
    auto& health = notifications.receiver;
    health.degraded = true;
    health.failures = 7;
    health.reopen_attempts = 3;
    health.first_failure = 123456789;
    const auto native = Error::from_raw(static_cast<ErrorDomain>(999), -1, -2,
        NativeError{static_cast<NativeErrorSource>(999), -123});
    health.last_error = wrap_error(module_system::ErrorCode::NotificationFailed,
        module_system::ErrorOperation::WaitNotification, native);
    for (std::size_t index = 0; index < config::error_chain_max_depth + 1; ++index) {
        health.last_error = wrap_error(module_system::ErrorCode::NotificationFailed,
            module_system::ErrorOperation::WaitNotification, *health.last_error);
    }
    notifications.local_sender = health;
    notifications.publisher_routes = health;
    ModulePage page;
    for (std::uint32_t index = 0; index < detail::modules_per_page; ++index) {
        page.modules.push_back({index + 1, std::string(detail::query_name_bytes, 'n'),
            std::string(detail::query_name_bytes, 't'), ModuleState::Running, notifications});
    }
    auto encoded = take(detail::encode_modules(page));
    check(encoded.size() + detail::result_tag_bytes <= control::max_payload, "notification page respects wire budget");
    auto decoded = take(detail::decode_modules(encoded, 0));
    check(decoded.modules.front().notifications.receiver.last_error == health.last_error,
        "notification diagnostics retain unknown native codes and causes");
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        check(!detail::decode_modules(std::span<const std::byte>(encoded).first(length), 0),
            "truncated health pages rejected");
    }
    auto obsolete = encoded;
    obsolete[3] = std::byte{1};
    check(!detail::decode_modules(obsolete, 0), "old query version rejected");
    control::Bytes report;
    detail::encode_notifications(report, notifications);
    report[3] = std::byte{2};
    control::Reader reader(report);
    check(!detail::decode_notifications(reader), "unknown notification report rejected");
}

void run() {
    auto service = take(control::Service::create(2));
    auto sockets = channel_test::connected_sockets();
    auto options = channel_test::options();
    auto central = take(service->attach(std::move(sockets.first), options));
    options.role = control::Role::Module;
    auto module = take(service->attach(std::move(sockets.second), options));
    check(bool(central->wait_open(deadline())), "central handshake");
    check(bool(module->wait_open(deadline())), "module handshake");
    detail::ModuleQueries queries(module);
    const auto exchange = [&](control::Bytes body) {
        auto waiting = std::async(std::launch::async, [&] { return queries.list_modules(0, deadline()); });
        auto frame = take(central->receive_until(deadline()));
        check(frame.operation == detail::list_modules, "read-only lifecycle operation");
        control::Frame reply{control::Kind::Response, detail::lifecycle, detail::list_modules, frame.request};
        reply.payload = std::move(body);
        check(bool(queries.receive(std::move(reply))), "reply delivered by sole receiver");
        return waiting.get();
    };
    control::Bytes success;
    control::put_u32(success, detail::result_success);
    auto empty = take(detail::encode_modules({}));
    success.insert(success.end(), empty.begin(), empty.end());
    check(take(exchange(success)).modules.empty(), "empty snapshot accepted");
    auto malformed = success;
    malformed.pop_back();
    auto rejected = exchange(malformed);
    check(!rejected && rejected.error().is(module_system::ErrorCode::Protocol), "truncated snapshot rejected");
    auto expired = queries.list_modules(0, control::Clock::now() + short_timeout);
    check(!expired && expired.error().is(module_system::ErrorCode::Timeout), "read timeout returned");
    auto late = take(central->receive_until(deadline()));
    check(bool(queries.receive({control::Kind::Response, detail::lifecycle, detail::list_modules, late.request, success})),
        "late reply discarded");
    check(take(exchange(success)).modules.empty(), "query remains usable after timeout");

    auto waiting = std::async(std::launch::async, [&] { return queries.list_modules(0, deadline()); });
    take(central->receive_until(deadline()));
    auto busy = queries.list_modules(0, deadline());
    check(!busy && busy.error().is(module_system::ErrorCode::Busy), "one query in flight");
    queries.stop();
    check(waiting.wait_for(timeout) == std::future_status::ready, "shutdown wakes pending query");
    auto cancelled = waiting.get();
    check(!cancelled && cancelled.error().is(module_system::ErrorCode::Cancelled), "shutdown cancellation");
}
}
int main() {
    try {
        notification_codec();
        run();
        std::cout << "Module read-only query tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
