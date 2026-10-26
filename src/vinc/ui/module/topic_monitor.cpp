#include <vinc/ui/detail/topic_monitor.hpp>
#include <vinc/ui/detail/topic_history.hpp>
#include <vinc/ui/detail/topic_cache.hpp>
#include <vinc/ui/error.hpp>
#include <vinc/time.hpp>
#include <vinc/message/detail/preview.hpp>
#include <vinc_messages/message_types.hpp>
#include <nlohmann/json.hpp>

namespace vinc::ui::detail {
namespace {
using Json = nlohmann::json;
using Clock = MonitorClock;

Json number(std::uint64_t value) {
    // IDs, timestamps and counters remain lossless in browser JavaScript.
    return std::to_string(value);
}

Json publisher_json(const message_bus::PublisherDiagnostics& publisher, std::optional<double> hz) {
    const auto& stats = publisher.stats;
    return Json{{"id", number(publisher.id)}, {"mid", number(publisher.owner)}, {"name", publisher.name},
        {"hz", hz ? Json(*hz) : Json(nullptr)}, {"slots", stats.effective_slots},
        {"requested_slots", stats.requested_slots}, {"message_bytes", number(stats.max_message_bytes)},
        {"shared_bytes", number(stats.shared_bytes)}, {"published", number(stats.published)},
        {"collisions", number(stats.collisions)}, {"wait_ns", number(stats.total_wait_ns)},
        {"longest_wait_ns", number(stats.longest_wait_ns)}, {"current_wait_ns", number(stats.current_wait_ns)}};
}

std::string type_name(std::uint64_t schema) {
    std::string name;
    message::visit_types([&]<class T> {
        if (T::schema().hash == schema) {
            name = T::message_name;
        }
    });
    return name;
}
}

struct TopicMonitor::Impl {
    Context& context;
    TopicCache cache;
    std::map<std::string, TopicHistory> history;
    std::map<std::string, message_bus::TopicDiagnostics> topics;
    struct Preview {
        std::uint64_t schema;
        std::function<Json()> read;
    };
    std::map<std::string, Preview> previews;
    Clock::time_point next_sample{};
    Clock::time_point next_preview{};

    explicit Impl(Context& owner) : context(owner) {
    }

    Result<void> discover() {
        std::map<std::string, message_bus::TopicDiagnostics> received;
        std::string cursor;
        do {
            auto page = context.inspect_topics(cursor);
            if (!page) {
                return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::InspectTopics, page.error()));
            }

            for (auto& topic : page->topics) {
                if (received.size() >= config::ui_monitor_topics) {
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::InspectTopics});
                }

                received.emplace(topic.name, std::move(topic));
            }

            cursor = std::move(page->next);
        } while (!cursor.empty() && context.living());

        topics = std::move(received);
        return {};
    }

    void sample(Clock::time_point now) {
        auto result = discover();
        Json data{{"apiVersion", 1}, {"available", static_cast<bool>(result)}, {"topics", Json::array()},
            {"sample_ms", std::chrono::duration_cast<std::chrono::milliseconds>(config::ui_topic_sample_interval).count()},
            {"preview_ms", config::ui_preview_interval.count()},
            {"policy", Json{{"rate_window_ms", std::chrono::duration_cast<std::chrono::milliseconds>(config::ui_topic_rate_window).count()},
                {"recovery_ms", std::chrono::duration_cast<std::chrono::milliseconds>(config::ui_topic_recovery).count()},
                {"red_wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(config::ui_topic_red_wait).count()},
                {"preview_bytes", config::ui_preview_message_bytes}, {"preview_text_bytes", config::ui_preview_text_bytes},
                {"preview_elements", config::ui_preview_elements}, {"preview_nodes", config::ui_preview_nodes},
                {"preview_depth", config::ui_preview_depth}, {"preview_string_bytes", config::ui_preview_string_bytes}}}};
        if (!result) {
            data["error"] = error_to_string(result.error());
        } else {
            std::erase_if(history, [&](const auto& item) {
                return !topics.contains(item.first);
            });
            for (const auto& [name, topic] : topics) {
                std::map<std::uint64_t, std::optional<double>> rates;
                const auto status = history[name].observe(topic, now, rates);
                Json publishers = Json::array();
                double hz = 0;
                bool measured = true;
                bool waiting = false;
                for (const auto& publisher : topic.publishers) {
                    const auto rate = rates.at(publisher.id);
                    if (rate) {
                        hz += *rate;
                    } else {
                        measured = false;
                    }
                    waiting = waiting || publisher.stats.current_wait_ns != 0;
                    publishers.push_back(publisher_json(publisher, rate));
                }

                const auto type = type_name(topic.schema);
                std::string reason = "No recent issue observed";
                if (history[name].read_failed) {
                    reason = "Message read failed";
                } else if (waiting) {
                    reason = "Writer waiting for reader copy";
                } else if (status == "yellow") {
                    reason = "Recent contention or subscriber loss; recovering";
                }
                data["topics"].push_back(Json{{"name", name}, {"schema", number(topic.schema)},
                    {"type", type.empty() ? "Unknown type" : type}, {"known_type", !type.empty()},
                    {"hz", measured ? Json(hz) : Json(nullptr)}, {"status", status}, {"reason", reason},
                    {"subscribers", topic.subscribers}, {"lost", number(topic.lost_messages)},
                    {"overwrites", number(topic.overwrite_events)}, {"oversized", number(topic.oversized_messages)},
                    {"publishers", std::move(publishers)}});
            }
        }

        auto text = data.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (text.size() > config::ui_topic_snapshot_bytes) {
            text = R"({"apiVersion":1,"available":false,"topics":[],"error":"Diagnostic snapshot exceeds UI budget"})";
        }

        cache.publish(std::move(text));
    }

    template<class T> Result<Preview> subscribe(const std::string& topic) {
        auto subscribed = context.subscribe<T>(topic,
            {.start = message_bus::StartPosition::NewOnly, .max_message_bytes = config::ui_preview_message_bytes});
        if (!subscribed) {
            return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Preview, subscribed.error()));
        }

        auto subscriber = std::make_shared<message_bus::Subscriber<T>>(std::move(*subscribed));
        return Preview{T::schema().hash, [subscriber]() -> Json {
            auto message = subscriber->get_last();
            if (!message) {
                const auto& error = message.error();
                if (error.is(message_bus::ErrorCode::NoPublishedData)) {
                    return Json{{"state", "empty"}};
                }
                if (error.is(message_bus::ErrorCode::CapacityExceeded)) {
                    return Json{{"state", "too_large"}, {"limit_bytes", number(config::ui_preview_message_bytes)}};
                }

                return Json{{"state", "error"}, {"error", error_to_string(
                    wrap_error(ErrorCode::BusFailed, ErrorOperation::Preview, error))}};
            }

            const auto& value = **message;
            auto text = message::detail::preview(value, {config::ui_preview_text_bytes,
                config::ui_preview_elements, config::ui_preview_nodes, config::ui_preview_depth,
                config::ui_preview_string_bytes});
            auto utc = format_utc(value.get_message_timestamp());
            return Json{{"state", "ready"}, {"publisher", value.publisher_name()},
                {"publisher_id", number(value.publisher_instance())},
                {"timestamp", number(value.get_message_timestamp())}, {"utc", utc},
                {"count", number(value.get_topic_count())}, {"text", std::move(text)},
                {"limits", "Bounded preview; arrays and strings may be truncated"}};
        }};
    }

    Json preview(const std::string& name) {
        const auto topic = topics.find(name);
        if (topic == topics.end() || topic->second.publishers.empty()) {
            previews.erase(name);
            return Json{{"state", "empty"}};
        }

        auto current = previews.find(name);
        if (current != previews.end() && current->second.schema != topic->second.schema) {
            previews.erase(current);
        }

        if (!previews.contains(name)) {
            bool known = false;
            Result<void> attached;
            message::visit_types([&]<class T> {
                if (!known && T::schema().hash == topic->second.schema) {
                    known = true;
                    auto result = subscribe<T>(name);
                    if (result) {
                        previews.emplace(name, std::move(*result));
                    } else {
                        attached = std::unexpected(result.error());
                    }
                }
            });
            if (!known) {
                return Json{{"state", "unknown"}};
            }
            if (!attached) {
                return Json{{"state", "error"}, {"error", error_to_string(attached.error())}};
            }
        }

        return previews.at(name).read();
    }

    void update_previews(Clock::time_point now) {
        const auto active = cache.active(now);

        std::erase_if(previews, [&](const auto& item) {
            return std::find(active.begin(), active.end(), item.first) == active.end();
        });
        for (const auto& name : active) {
            auto data = preview(name);
            data["apiVersion"] = 1;
            const bool failed = data["state"] == "error";
            if (auto entry = history.find(name); entry != history.end()) {
                entry->second.read_result(failed, now);
            }

            auto text = data.dump(-1, ' ', false, Json::error_handler_t::replace);
            cache.publish_preview(name, std::move(text));
        }
    }
};

TopicMonitor::TopicMonitor(Context& context) : impl_(std::make_unique<Impl>(context)) {
}
TopicMonitor::~TopicMonitor() = default;

void TopicMonitor::poll() {
    const auto now = Clock::now();
    if (now >= impl_->next_sample) {
        impl_->sample(now);
        impl_->next_sample = Clock::now() + config::ui_topic_sample_interval;
    }

    if (now >= impl_->next_preview) {
        impl_->update_previews(now);
        impl_->next_preview = Clock::now() + config::ui_preview_interval;
    }
}

Result<std::string> TopicMonitor::read(const std::string& topic) {
    return impl_->cache.read(topic);
}
}
