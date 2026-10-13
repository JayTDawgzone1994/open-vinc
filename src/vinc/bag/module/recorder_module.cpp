#include <vinc/bag/bag.hpp>
#include <vinc/time.hpp>
#include <vinc/module/base_module.hpp>
#include <vinc/osal/path.hpp>
#include <vinc_messages/message_types.hpp>
#include <charconv>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace {
using namespace vinc;
using namespace vinc::bag;

struct Subscription {
    std::uint64_t schema = 0;
    std::function<message_bus::SubscriberStats()> stats;
    std::function<void()> close;
    std::uint64_t lost = 0;
    std::uint64_t oversized = 0;
};

class RecorderModule final : public BaseModule<RunMode::Manual> {
    std::unique_ptr<Recorder> recorder_;
    std::map<std::string, Subscription> subscriptions_;
    std::set<std::string> selected_;
    std::set<std::pair<std::string, std::uint64_t>> unsupported_;
    std::optional<Error> callback_failure_;
    Options options_;
    bool all_ = false;
    bool failure_reported_ = false;
    std::chrono::steady_clock::time_point next_discovery_{};

    void losses(Subscription& subscription) {
        const auto stats = subscription.stats();
        recorder_->observe_losses(stats.lost_messages - subscription.lost,
                                  stats.oversized_messages - subscription.oversized);
        subscription.lost = stats.lost_messages;
        subscription.oversized = stats.oversized_messages;
    }

    void finish() {
        for (auto& [name, subscription] : subscriptions_) {
            losses(subscription);
            subscription.close();
        }

        subscriptions_.clear();
        if (recorder_) {
            recorder_->finish(callback_failure_);
        }
    }

    template <class T> Result<Subscription> subscribe(const std::string& topic) {
        auto subscribed = context.subscribe<T>(
            topic,
            [this](std::unique_ptr<T> message) {
                auto usage = message->usage();
                if (!usage) {
                    callback_failure_ =
                        wrap_error(ErrorCode::StorageFailed, ErrorOperation::Enqueue, usage.error());
                    return;
                }

                auto result = recorder_->enqueue(std::move(message), usage->allocated_bytes,
                                                 [](const BaseMessage& value, std::uint64_t maximum) {
                                                     return static_cast<const T&>(value).snapshot(maximum);
                                                 });
                if (!result && !result.error().is(ErrorCode::CapacityExceeded) &&
                    !result.error().is(ErrorCode::Closed)) {
                    callback_failure_ = result.error();
                }
            },
            {.start = message_bus::StartPosition::NewOnly, .max_message_bytes = options_.message_bytes});
        if (!subscribed) {
            return std::unexpected(
                wrap_error(ErrorCode::BusFailed, ErrorOperation::Discover, subscribed.error()));
        }

        auto owner = std::make_shared<message_bus::Subscriber<T>>(std::move(*subscribed));
        return Subscription{T::schema().hash,
                            [owner] {
                                return owner->stats();
                            },
                            [owner] {
                                owner->close();
                            }};
    }

    Result<void> discover() {
        std::map<std::string, message_bus::TopicInfo> topics;
        std::string cursor;
        do {
            auto page = context.list_topics(cursor);
            if (!page) {
                return std::unexpected(
                    wrap_error(ErrorCode::BusFailed, ErrorOperation::Discover, page.error()));
            }

            for (auto& topic : page->topics) {
                if (topics.size() >= config::bag_max_topics) {
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Discover});
                }

                topics.emplace(topic.name, std::move(topic));
            }

            cursor = std::move(page->next);
        } while (!cursor.empty());
        for (auto iterator = subscriptions_.begin(); iterator != subscriptions_.end();) {
            auto present = topics.find(iterator->first);
            if (present == topics.end() || present->second.schema != iterator->second.schema) {
                losses(iterator->second);
                iterator->second.close();
                iterator = subscriptions_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        for (const auto& [name, topic] : topics) {
            if ((!all_ && !selected_.contains(name)) || subscriptions_.contains(name)) {
                continue;
            }

            bool matched = false;
            Result<void> attached;
            message::visit_types([&]<class T> {
                if (matched || T::schema().hash != topic.schema) {
                    return;
                }

                matched = true;
                attached = recorder_->discover({name, std::string(T::message_name), topic.schema, true});
                if (!attached) {
                    return;
                }

                auto subscription = subscribe<T>(name);
                if (!subscription) {
                    attached = std::unexpected(subscription.error());
                    return;
                }

                subscriptions_.emplace(name, std::move(*subscription));
            });
            if (!attached) {
                return attached;
            }

            if (!matched) {
                auto saved = recorder_->discover({name, "", topic.schema, false});
                if (!saved) {
                    return saved;
                }

                if (unsupported_.emplace(name, topic.schema).second) {
                    std::cout << "bag: " << name << " hash=" << topic.schema
                              << " unsupported type (not recorded)\n"
                              << std::flush;
                }
            }
        }

        return {};
    }

    Result<void> record(const std::vector<std::string>& args) {
        if (args.size() < 3 || (recorder_ && !recorder_->status().finished)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
        }

        Options options;
        std::set<std::string> selected;
        bool all = false;
        for (std::size_t index = 2; index < args.size(); ++index) {
            if (args[index] == "--all") {
                all = true;
            } else if (args[index] == "--topic" && index + 1 < args.size()) {
                const auto& name = args[++index];
                if (name.empty() || name.size() > config::bag_name_bytes ||
                    name.find('.') != std::string::npos || name.find('\0') != std::string::npos) {
                    return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
                }

                selected.insert(name);
            } else if ((args[index] == "--database-bytes" || args[index] == "--queue-bytes" ||
                        args[index] == "--message-bytes") &&
                       index + 1 < args.size()) {
                const auto option = args[index++];
                std::uint64_t value = 0;
                const auto& text = args[index];
                auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !value) {
                    return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
                }

                if (option == "--database-bytes") {
                    options.database_bytes = value;
                } else if (option == "--queue-bytes") {
                    options.queue_bytes = value;
                } else {
                    options.message_bytes = value;
                }
            } else {
                return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
            }
        }

        const bool missing_selection = !all && selected.empty();
        const bool conflicting_selection = all && !selected.empty();
        if (missing_selection || conflicting_selection || selected.size() > config::bag_max_topics) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
        }

        auto path = osal::path_from_utf8(args[1]);
        if (!path) {
            return std::unexpected(wrap_error(ErrorCode::FileFailed, ErrorOperation::Command, path.error()));
        }

        // A failed worker can finish between the main-loop check and this command.
        // Retire its subscriptions before replacing the callback's Recorder owner.
        if (recorder_) {
            const auto previous = recorder_->status();
            if (previous.failure && !failure_reported_) {
                std::cerr << "bag: previous recording failed: " << error_to_string(*previous.failure) << '\n';
            }

            finish();
        }

        auto created = Recorder::create(*path, options);
        if (!created) {
            return std::unexpected(created.error());
        }

        recorder_ = std::move(*created);
        options_ = options;
        selected_ = std::move(selected);
        all_ = all;
        unsupported_.clear();
        callback_failure_.reset();
        failure_reported_ = false;
        next_discovery_ = {};
        std::cout << "bag: recording " << args[1] << '\n' << std::flush;
        return {};
    }

    Result<void> info(const std::string& directory) {
        auto path = osal::path_from_utf8(directory);
        if (!path) {
            return std::unexpected(wrap_error(ErrorCode::FileFailed, ErrorOperation::Inspect, path.error()));
        }

        auto result = inspect(*path);
        if (!result) {
            return std::unexpected(result.error());
        }

        std::cout << "bag: completed=" << result->completed << " written=" << result->counters.written
                  << " queue_dropped=" << result->counters.queue_dropped
                  << " bus_lost=" << result->counters.bus_lost << " oversized=" << result->counters.oversized
                  << " start_utc_ns=" << result->start_utc_ns << '\n';
        for (const auto& entry : result->topics) {
            bool known = false;
            bool same_name = false;
            message::visit_types([&]<class T> {
                known = known || T::schema().hash == entry.topic.schema;
                same_name = same_name || T::message_name == entry.topic.type;
            });
            const char* state = "unknown type";
            if (known) {
                state = "known type";
            } else if (same_name) {
                state = "schema mismatch";
            }

            std::cout << entry.topic.name << " type=" << entry.topic.type << " hash=" << entry.topic.schema
                      << " supported_at_recording=" << entry.topic.supported << " records=" << entry.records
                      << " first_utc=" << vinc::format_utc(entry.first_timestamp)
                      << " last_utc=" << vinc::format_utc(entry.last_timestamp) << " "
                      << state << '\n';
        }

        for (const auto& source : result->sources) {
            std::cout << "publisher=" << source.publisher << " publisher_id=" << source.publisher_id
                      << " records=" << source.records << '\n';
        }

        std::cout.flush();
        return {};
    }

public:
    RecorderModule() : BaseModule("recorder") {
    }

    Result<std::string> on_name(std::vector<std::string>& args) override {
        // Shell includes the ModuleType target; direct Manager callers may omit it.
        if (!args.empty() && args.front() == "bagRecorder") {
            args.erase(args.begin());
        }

        auto name = get_module_name();
        if (args.size() >= 2 && args.front() == "--name") {
            name = args[1];
            args.erase(args.begin(), args.begin() + 2);
        }

        return name;
    }

    Result<void> on_start(const std::vector<std::string>& args) override {
        if (args.empty()) {
            return {};
        }

        if (args.front() == "record") {
            return record(args);
        }

        if (args.size() == 2 && args.front() == "info") {
            return info(args[1]);
        }

        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
    }

    Result<void> main() override {
        while (context.living()) {
            if (recorder_ && recorder_->status().accepting) {
                if (std::chrono::steady_clock::now() >= next_discovery_) {
                    auto discovered = discover();
                    next_discovery_ = std::chrono::steady_clock::now() + config::bag_discovery_interval;
                    if (!discovered) {
                        callback_failure_ = discovered.error();
                    }
                }

                for (auto& [name, subscription] : subscriptions_) {
                    losses(subscription);
                }
            }

            if (recorder_) {
                auto state = recorder_->status();
                if (callback_failure_ || state.failure) {
                    finish();
                    if (!failure_reported_) {
                        std::cerr << "bag: recording stopped: "
                                  << error_to_string(callback_failure_ ? *callback_failure_ : *state.failure)
                                  << '\n';
                        failure_reported_ = true;
                    }
                }
            }

            auto pumped = context.pumpOnce();
            if (!pumped) {
                callback_failure_ = wrap_error(ErrorCode::BusFailed, ErrorOperation::Enqueue, pumped.error());
                return std::unexpected(*callback_failure_);
            }

            std::this_thread::sleep_for(config::bag_pump_interval);
        }

        return {};
    }

    Result<void> on_stop() override {
        finish();
        if (recorder_) {
            return recorder_->wait();
        }

        return {};
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        if (args.empty()) {
            std::cout << "bagRecorder: record DIR (--all | --topic NAME...) [--database-bytes N] "
                         "[--queue-bytes N] [--message-bytes N]; status; finish; info DIR\n";
            std::cout.flush();
            return 0;
        }

        if (args[0] == "record") {
            auto result = record(args);
            if (!result) {
                return std::unexpected(result.error());
            }
        } else if (args[0] == "finish" && args.size() == 1) {
            finish();
        } else if (args[0] == "status" && args.size() == 1) {
            if (!recorder_) {
                std::cout << "bag: idle\n";
            } else {
                const auto state = recorder_->status();
                std::cout << "bag: accepting=" << state.accepting << " finished=" << state.finished
                          << " written=" << state.counters.written
                          << " queue_dropped=" << state.counters.queue_dropped
                          << " bus_lost=" << state.counters.bus_lost
                          << " oversized=" << state.counters.oversized
                          << " pending_bytes=" << state.pending_bytes
                          << " unsupported_topics=" << unsupported_.size()
                          << " failed=" << (state.failure.has_value() || callback_failure_.has_value())
                          << '\n';
            }
        } else if (args[0] == "info" && args.size() == 2) {
            auto result = info(args[1]);
            if (!result) {
                return std::unexpected(result.error());
            }
        } else {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
        }

        // A successful command reply must not leave its text in the child CRT buffer.
        std::cout.flush();
        return 0;
    }
};
}
REGISTER_MODULE(RecorderModule, "bagRecorder")
