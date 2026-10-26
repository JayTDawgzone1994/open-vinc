#include <vinc/ui/detail/topic_history.hpp>
#include <vinc/ui/detail/topic_cache.hpp>
#include <vinc/message_bus/detail/diagnostics_protocol.hpp>
#include <vinc/message/detail/preview.hpp>
#include <vinc_messages/UserExampleFrame.hpp>
#include <iostream>

namespace {
bool check(bool condition, const char* text) {
    if (!condition) {
        std::cerr << "FAIL: " << text << '\n';
    }
    return condition;
}

bool history() {
    using namespace std::chrono_literals;
    using namespace vinc::ui::detail;
    TopicHistory history;
    vinc::message_bus::TopicDiagnostics topic;
    topic.name = "test/topic";
    topic.schema = 1;
    topic.publishers.push_back({1, 1, "sender", {2, 2, 4096, 16384}});
    auto& stats = topic.publishers.front().stats;
    std::map<std::uint64_t, std::optional<double>> rates;
    auto now = MonitorClock::time_point{} + 10s;
    if (!check(history.observe(topic, now, rates) == "green" && !rates[1], "first rate is unknown")) {
        return false;
    }

    stats.published = 10;
    now += 1s;
    if (!check(history.observe(topic, now, rates) == "green" && rates[1] == 10.0, "rate uses publication delta")) {
        return false;
    }

    stats.collisions = 1;
    stats.current_wait_ns = 1;
    now += 1s;
    if (!check(history.observe(topic, now, rates) == "yellow", "short active wait is yellow")) {
        return false;
    }

    stats.current_wait_ns = 1'000'000'000;
    now += 1s;
    if (!check(history.observe(topic, now, rates) == "red", "one second current wait is red")) {
        return false;
    }

    stats.current_wait_ns = 0;
    now += 1s;
    if (!check(history.observe(topic, now, rates) == "yellow", "wait recovery remains yellow")) {
        return false;
    }

    now += 5s;
    if (!check(history.observe(topic, now, rates) == "green", "historical collisions do not latch yellow")) {
        return false;
    }

    topic.lost_messages = 1;
    if (!check(history.observe(topic, now, rates) == "yellow", "reader loss without writer collision is visible")) {
        return false;
    }

    history.read_result(true, now);
    if (!check(history.observe(topic, now, rates) == "red", "read failure is red")) {
        return false;
    }
    history.read_result(false, now);
    now += 5s;
    if (!check(history.observe(topic, now, rates) == "green", "read failure recovery")) {
        return false;
    }

    topic.publishers.front().id = 2;
    stats.published = 0;
    rates.clear();
    return check(history.observe(topic, now + 1s, rates) == "green" && !rates[2] &&
        history.publishers.size() == 1, "publisher restart does not produce negative rate or retain history");
}

bool codec() {
    using namespace vinc::message_bus;
    TopicDiagnosticsPage page;
    TopicDiagnostics topic;
    topic.name = "test/topic";
    topic.schema = UINT64_MAX;
    topic.subscribers = 2;
    topic.lost_messages = UINT64_MAX;
    topic.publishers.push_back({1, 2, "sender", {1, 2, 4096, 16384, UINT64_MAX, 12, 14, 15, 16}});
    page.topics.push_back(topic);
    const auto encoded = detail::encode_diagnostics(page);
    if (!check(static_cast<bool>(encoded), "encode diagnostics")) {
        return false;
    }
    const auto decoded = detail::decode_diagnostics(*encoded, "");
    if (!check(decoded && decoded->topics[0].lost_messages == UINT64_MAX &&
        decoded->topics[0].publishers[0].stats.published == UINT64_MAX, "lossless 64-bit diagnostic codec")) {
        return false;
    }

    auto maximum = page;
    maximum.topics[0].name.assign(detail::maximum_name_bytes, 't');
    maximum.topics[0].publishers.clear();
    for (std::uint64_t id = 1; id <= detail::maximum_attached_publishers; ++id) {
        auto publisher = topic.publishers.front();
        publisher.id = id;
        publisher.name.assign(detail::maximum_name_bytes, 'p');
        maximum.topics[0].publishers.push_back(std::move(publisher));
    }
    const auto largest = detail::encode_diagnostics(maximum);
    if (!check(largest && largest->size() + detail::response_status_bytes <= vinc::control::max_payload &&
        detail::decode_diagnostics(*largest, ""), "maximum publisher page fits the existing control frame")) {
        return false;
    }

    for (std::size_t size = 0; size < encoded->size(); ++size) {
        if (!check(!detail::decode_diagnostics(std::span(*encoded).first(size), ""), "truncated diagnostics rejected")) {
            return false;
        }
    }
    auto malformed = *encoded;
    malformed.push_back(std::byte{0});
    if (!check(!detail::decode_diagnostics(malformed, ""), "trailing data rejected")) {
        return false;
    }
    malformed = *encoded;
    malformed[3] = std::byte{2};
    if (!check(!detail::decode_diagnostics(malformed, ""), "unknown diagnostic version rejected")) {
        return false;
    }
    return check(!detail::decode_diagnostics(*encoded, "test/topic"), "non-advancing page rejected");
}

bool cache() {
    using namespace std::chrono_literals;
    using vinc::ui::detail::TopicCache;
    TopicCache cache;
    const auto now = TopicCache::Clock::time_point{} + 20s;
    cache.publish(R"({"apiVersion":1,"available":true,"topics":[]})", now);
    auto pending = cache.read("camera/frame", now);
    if (!check(pending && pending->find("pending") != pending->npos, "preview demand begins pending")) {
        return false;
    }

    cache.publish_preview("camera/frame", R"({"apiVersion":1,"state":"ready","text":"test"})", now);
    auto fresh = cache.read("camera/frame", now + 1s);
    if (!check(fresh && fresh->find("ready") != fresh->npos, "fresh preview cache")) {
        return false;
    }

    auto stale = cache.read("", now + vinc::config::ui_topic_stale_timeout);
    if (!check(stale && stale->find("\"available\":false") != stale->npos, "stalled main cannot serve cached green as live")) {
        return false;
    }

    const auto later = now + vinc::config::ui_topic_stale_timeout;
    cache.publish(R"({"apiVersion":1,"available":true,"topics":[]})", later);
    auto stale_preview = cache.read("camera/frame", later);
    if (!check(stale_preview && stale_preview->find("unavailable") != stale_preview->npos,
        "preview freshness independent of list freshness")) {
        return false;
    }

    if (!check(cache.active(later + vinc::config::ui_preview_idle_timeout).empty(), "idle preview demand is reclaimed")) {
        return false;
    }

    cache.publish(R"({"available":true})", later);
    for (std::size_t index = 0; index < vinc::config::ui_preview_topics; ++index) {
        if (!cache.read("topic/" + std::to_string(index), later)) {
            return false;
        }
    }

    const auto full = cache.read("excess", later);
    return check(!full && full.error().is(vinc::ui::ErrorCode::CapacityExceeded), "preview demands are bounded");
}

bool preview() {
    auto message = vinc::make_message<vinc::UserExampleFrame>();
    message->data().sequence = UINT64_MAX;
    if (!message->data().note.assign("<script>not HTML</script>") || !message->data().samples.resize(1000) ||
        !message->data().groups.resize(1) || !message->data().groups[0].resize(1)) {
        return false;
    }
    auto item = message->data().groups[0][0];
    item.data().id = 42;
    if (!item.data().label.assign("nested")) {
        return false;
    }

    using vinc::message::detail::preview;
    const auto text = preview(*message, {4096, 4, 64, 16, 64});
    if (!check(text.find("18446744073709551615") != text.npos &&
        text.find("size=1000") != text.npos && text.find("nested") != text.npos,
        "bounded preview supports scalars, strings and nested records")) {
        return false;
    }

    const auto bounded = preview(*message, {32, 4, 64, 16, 64});
    return check(bounded.size() <= 32 && bounded.ends_with("..."), "preview output budget enforced");
}
}

int main() {
    if (!history() || !codec() || !preview() || !cache()) {
        return 1;
    }
    std::cout << "Topic history, wire validation and bounded message preview passed\n";
    return 0;
}
