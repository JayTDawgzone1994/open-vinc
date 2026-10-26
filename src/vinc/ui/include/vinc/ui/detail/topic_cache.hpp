#pragma once

// The HTTP/main-thread seam owns no Context, Subscriber or worker. Freshness is
// measured locally, so a responsive HTTP listener cannot present stalled samples as live.
#include <vinc/ui/error.hpp>
#include <vinc/config.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vinc::ui::detail {
class TopicCache {
public:
    using Clock = std::chrono::steady_clock;

private:
    struct Demand {
        Clock::time_point touched;
        std::optional<Clock::time_point> updated;
        std::string reply = R"({"apiVersion":1,"state":"pending"})";
    };
    std::mutex mutex_;
    std::string snapshot_ = R"({"apiVersion":1,"available":false,"topics":[],"error":"Waiting for first sample"})";
    std::optional<Clock::time_point> updated_;
    std::map<std::string, Demand> demands_;

public:
    void publish(std::string snapshot, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
        updated_ = now;
    }

    Result<std::string> read(const std::string& topic, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        const bool stale = updated_ && now - *updated_ >= config::ui_topic_stale_timeout;
        if (topic.empty()) {
            if (stale) {
                return std::string(R"({"apiVersion":1,"available":false,"topics":[],"error":"Diagnostic samples are stale"})");
            }

            return snapshot_;
        }
        if (stale) {
            return std::string(R"({"apiVersion":1,"state":"unavailable"})");
        }

        if (topic.size() > config::ui_topic_name_bytes || topic.find('\0') != topic.npos || topic.find('.') != topic.npos) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Preview});
        }

        auto present = demands_.find(topic);
        if (present == demands_.end()) {
            if (demands_.size() >= config::ui_preview_topics) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Preview});
            }

            present = demands_.emplace(topic, Demand{.touched = now}).first;
        }

        auto& demand = present->second;
        demand.touched = now;
        if (demand.updated && now - *demand.updated >= config::ui_topic_stale_timeout) {
            return std::string(R"({"apiVersion":1,"state":"unavailable"})");
        }

        return demand.reply;
    }

    std::vector<std::string> active(Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        std::erase_if(demands_, [&](const auto& item) {
            return now - item.second.touched >= config::ui_preview_idle_timeout;
        });
        std::vector<std::string> result;
        for (const auto& [name, _] : demands_) {
            result.push_back(name);
        }

        return result;
    }

    void publish_preview(const std::string& name, std::string reply, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        if (auto found = demands_.find(name); found != demands_.end()) {
            found->second.reply = std::move(reply);
            found->second.updated = now;
        }
    }
};
}
