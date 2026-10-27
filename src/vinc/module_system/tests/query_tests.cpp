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
        run();
        std::cout << "Module read-only query tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
