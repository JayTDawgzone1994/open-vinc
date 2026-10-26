#pragma once

#include <vinc/message_bus/message_bus.hpp>
#include <deque>
#include <map>
#include <optional>

namespace vinc::ui::detail {
using MonitorClock = std::chrono::steady_clock;

struct RateHistory {
    struct Sample {
        MonitorClock::time_point time;
        std::uint64_t published;
    };
    std::deque<Sample> samples;
    std::uint64_t collisions = 0;

    std::optional<double> observe(const message_bus::PublisherStats& stats, MonitorClock::time_point now) {
        if (!samples.empty() && stats.published < samples.back().published) {
            samples.clear();
        }

        samples.push_back({now, stats.published});
        while (samples.size() > 2 && now - samples[1].time >= config::ui_topic_rate_window) {
            samples.pop_front();
        }
        while (samples.size() > config::ui_rate_samples) {
            samples.pop_front();
        }

        const auto elapsed = std::chrono::duration<double>(now - samples.front().time).count();
        if (elapsed <= 0) {
            return std::nullopt;
        }

        return static_cast<double>(stats.published - samples.front().published) / elapsed;
    }
};

struct TopicHistory {
    std::map<std::uint64_t, RateHistory> publishers;
    std::optional<MonitorClock::time_point> problem;
    std::uint64_t lost = 0;
    std::uint64_t oversized = 0;
    bool sampled = false;
    bool read_failed = false;
    bool was_waiting = false;

    void read_result(bool failed, MonitorClock::time_point now) {
        if (failed || read_failed) {
            problem = now;
        }
        read_failed = failed;
    }

    // Returns a display status, never permission to reclaim a reader or kill a module.
    std::string observe(const message_bus::TopicDiagnostics& topic, MonitorClock::time_point now,
                        std::map<std::uint64_t, std::optional<double>>& rates) {
        rates.clear();
        bool waiting = false;
        bool blocked = false;
        if (sampled && (topic.lost_messages > lost || topic.oversized_messages > oversized)) {
            problem = now;
        }
        lost = topic.lost_messages;
        oversized = topic.oversized_messages;
        for (const auto& publisher : topic.publishers) {
            auto [entry, added] = publishers.try_emplace(publisher.id);
            auto& history = entry->second;
            if ((!added && publisher.stats.collisions > history.collisions) || publisher.stats.current_wait_ns) {
                problem = now;
            }

            history.collisions = publisher.stats.collisions;
            rates[publisher.id] = history.observe(publisher.stats, now);
            waiting = waiting || publisher.stats.current_wait_ns != 0;
            blocked = blocked || publisher.stats.current_wait_ns >=
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(config::ui_topic_red_wait).count());
        }

        std::erase_if(publishers, [&](const auto& entry) {
            return !rates.contains(entry.first);
        });
        if (was_waiting && !waiting) {
            problem = now;
        }
        was_waiting = waiting;
        sampled = true;
        if (read_failed || blocked) {
            return "red";
        }

        if (waiting || (problem && now - *problem < config::ui_topic_recovery)) {
            return "yellow";
        }

        return "green";
    }
};
}
