#include <vinc/ui/detail/graph.hpp>
#include <vinc/module/detail/module_query_protocol.hpp>
#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto test_wait_timeout = std::chrono::seconds{3};
constexpr std::size_t control_envelope_bytes = 16 * 1024;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class T>
T take(vinc::Result<T> result) {
    check(result.has_value(), "expected successful result");
    return std::move(*result);
}
void codecs() {
    namespace bus = vinc::message_bus;
    bus::TopicConnectionsPage page;
    for (std::uint64_t index = 1; index <= bus::detail::connections_per_page; ++index) {
        page.connections.push_back({{std::string(bus::detail::maximum_name_bytes, 't'),
            bus::ConnectionRole::Subscriber, index}, 42, 1});
    }
    page.next = page.connections.back().key;
    auto encoded = take(bus::detail::encode_connections(page));
    check(encoded.size() < control_envelope_bytes, "worst-case connections page fits control envelope");
    auto decoded = take(bus::detail::decode_connections(encoded, {}));
    check(decoded.connections.size() == page.connections.size() && decoded.next == page.next, "connection page round trip");
    for (std::size_t size = 0; size < encoded.size(); ++size) {
        check(!bus::detail::decode_connections(std::span(encoded).first(size), {}), "truncated connection page rejected");
    }
    encoded.push_back(std::byte{0});
    check(!bus::detail::decode_connections(encoded, {}), "trailing connection bytes rejected");
    encoded.pop_back();
    encoded[0] = std::byte{99};
    check(!bus::detail::decode_connections(encoded, {}), "unknown connection version rejected");
    page.connections[1].key.id = page.connections[0].key.id;
    check(!bus::detail::decode_connections(take(bus::detail::encode_connections(page)), {}), "duplicate cursor rejected");

    vinc::ModulePage modules;
    for (std::uint64_t index = 1; index <= vinc::detail::modules_per_page; ++index) {
        modules.modules.push_back({index, std::string(vinc::detail::query_name_bytes, 'n'),
            std::string(vinc::detail::query_name_bytes, 't'), vinc::ModuleState::Running});
    }
    modules.next = modules.modules.back().mid;
    auto bytes = take(vinc::detail::encode_modules(modules));
    check(bytes.size() < control_envelope_bytes, "module page fits control envelope");
    check(take(vinc::detail::decode_modules(bytes, 0)).modules.size() == modules.modules.size(), "module page round trip");
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        check(!vinc::detail::decode_modules(std::span(bytes).first(size), 0), "truncated module page rejected");
    }
    modules.next = 1;
    check(!vinc::detail::decode_modules(take(vinc::detail::encode_modules(modules)), 0), "nonadvancing module cursor rejected");
}
void mailbox() {
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    unsigned calls = 0;
    vinc::ui::detail::GraphRequests requests([&](vinc::Deadline) -> vinc::Result<std::string> {
        ++calls;
        entered.set_value();
        released.wait();
        return "complete graph";
    });
    requests.process();
    check(calls == 0, "idle process does not collect graph");
    auto caller = std::async(std::launch::async, [&] { return requests.request(); });
    // The process thread polls the local mailbox just as UI main does; no network polling.
    std::jthread main([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            requests.process();
            std::this_thread::yield();
        }
    });
    auto waiting = entered.get_future();
    check(waiting.wait_for(test_wait_timeout) == std::future_status::ready, "query admitted");
    auto busy = requests.request();
    check(!busy && busy.error().is(vinc::ui::ErrorCode::Busy), "second graph query is bounded Busy");
    requests.stop();
    check(caller.wait_for(test_wait_timeout) == std::future_status::ready, "stop wakes HTTP waiter before join");
    auto closed = caller.get();
    check(!closed && closed.error().is(vinc::ui::ErrorCode::Closing), "shutdown reported");
    release.set_value();
    main.request_stop();
}
}
int main() {
    try {
        codecs();
        mailbox();
        std::cout << "Graph codec and mailbox tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
