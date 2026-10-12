#include <vinc/vinc.hpp>
#include "message_examples.hpp"

#include <charconv>
#include <cstdio>
#include <iostream>
#include <optional>

namespace {
using Message = vinc::UserExampleFrame;
constexpr std::uint32_t example_slots = 8;
constexpr std::uint64_t message_limit_bytes = 64 * 1024;
constexpr double initial_gain = 1.0;

// Application diagnostics deliberately use raw stderr if the structured log failed.
void report(const vinc::Error& error) {
    std::cerr << vinc::error_to_string(error) << '\n';
}
}

class UserExampleModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        for (auto argument = args.begin(); argument != args.end(); ++argument) {
            if (*argument != "--name") {
                continue;
            }

            if (argument + 1 == args.end()) {
                return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                                   vinc::module_system::ErrorOperation::NameHook});
            }

            auto name = *(argument + 1);
            args.erase(argument, argument + 2);
            return name;
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>& input) override {
        auto args = input;
        // Native Shell currently includes the target; Python's bridge removes it.
        if (!args.empty() && args.front() == "userExampleModule") {
            args.erase(args.begin());
        }

        if (!args.empty()) {
            if (args.size() != 2 || args[0] != "--topic") {
                return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                                   vinc::module_system::ErrorOperation::StartHook});
            }

            topic_ = args[1];
        }

        auto params = register_parameters();
        if (!params) {
            return start_failure(params.error());
        }

        auto publisher = open_publisher();
        if (!publisher) {
            return start_failure(publisher.error());
        }

        auto subscribers = open_subscribers();
        if (!subscribers) {
            return start_failure(subscribers.error());
        }

        std::cout << "READY " << get_module_name() << " mid=" << get_mid() << " topic=" << topic_ << '\n';
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        // A failed interactive command reports status 1, leaving this Module running.
        auto result = execute(args);
        if (!result) {
            report(result.error());
            return 1;
        }

        return 0;
    }

    vinc::Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) override {
        return std::vector<std::string>{"help", "status", "push", "loan", "last", "topics",
                                        "storage", "capacity", "params", "set", "logs", "reopen"};
    }

    vinc::Result<void> on_stop() override {
        // Startup can fail partway through. Optional handles make cleanup partial-safe.
        close_bus();
        retained_.reset();
        std::cout << "STOPPED " << get_module_name() << '\n';
        return {};
    }

private:
    std::string topic_ = "userExample/frames";
    double gain_ = initial_gain;
    std::optional<vinc::message_bus::Publisher<Message>> publisher_;
    std::optional<vinc::message_bus::Subscriber<Message>> subscriber_;
    std::optional<vinc::message_bus::Subscriber<Message>> latest_;
    std::unique_ptr<Message> retained_;

    static vinc::Result<void> start_failure(const vinc::Error& cause) {
        return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                                               vinc::module_system::ErrorOperation::StartHook, cause));
    }

    vinc::Result<void> register_parameters() {
        auto gain = context.param.register_param<double>("gain", initial_gain, vinc::param::Permission::Open);
        if (!gain) {
            return gain;
        }

        // Omitted permission means only the central framework can modify it.
        auto label = context.param.register_param<std::string>("label", "example");
        if (!label) {
            return label;
        }

        auto locked = context.param.register_param<double>("locked_gain", initial_gain);
        if (!locked) {
            return locked;
        }

        auto watched = context.param.register_param<double>("peer.gain");
        if (!watched) {
            return watched;
        }

        return context.param.on_change([this](const vinc::param::ParamMap& values) {
            // The reference is valid only during this callback. Copy values to retain them.
            auto found = values.find(get_module_name() + ".gain");
            if (found != values.end()) {
                auto gain = found->second.get<double>();
                if (gain) {
                    gain_ = *gain;
                    std::cout << "PARAM " << get_module_name() << " gain=" << gain_
                              << " version=" << found->second.version << '\n';
                } else {
                    report(gain.error());
                }
            }

            // An absent watched parameter has no fabricated Entry in the map.
            if (values.find("peer.gain") == values.end()) {
                std::cout << "WATCH peer.gain missing\n";
            }
        });
    }

    vinc::Result<void> open_publisher() {
        auto publisher = context.advertise<Message>(topic_, {
            .slots = example_slots, .max_message_bytes = message_limit_bytes});
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publisher_.emplace(std::move(*publisher));
        return {};
    }

    vinc::Result<void> open_subscribers() {
        auto subscriber = context.subscribe<Message>(topic_, [this](std::unique_ptr<Message> message) {
            // Ownership has already moved to local memory; no shared read lock remains.
            std::cout << "CALLBACK " << get_module_name() << ' ';
            user_example::describe(*message);
            retained_ = std::move(message);
        }, {.start = vinc::message_bus::StartPosition::OldestRetained,
            .max_message_bytes = message_limit_bytes});
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscriber_.emplace(std::move(*subscriber));
        // Without a callback this handle is for explicitly pulling the latest value.
        auto latest = context.subscribe<Message>(topic_, {
            .start = vinc::message_bus::StartPosition::NewOnly, .max_message_bytes = message_limit_bytes});
        if (!latest) {
            return std::unexpected(latest.error());
        }

        latest_.emplace(std::move(*latest));
        return {};
    }

    void close_bus() {
        if (subscriber_) {
            subscriber_->close();
        }

        if (latest_) {
            latest_->close();
        }

        if (publisher_) {
            publisher_->close();
        }

        subscriber_.reset();
        latest_.reset();
        publisher_.reset();
    }

    vinc::Result<void> publish(std::uint64_t sequence, bool use_loan) {
        if (!publisher_) {
            std::cout << "Publisher closed; run reopen first\n";
            return invalid_command();
        }

        if (!use_loan) {
            auto local = vinc::make_message<Message>();
            auto filled = user_example::fill(*local, sequence);
            if (!filled) {
                return filled;
            }

            return publisher_->push(*local);
        }

        auto loan = publisher_->get_next();
        if (!loan) {
            return std::unexpected(loan.error());
        }

        auto filled = user_example::fill(**loan, sequence);
        if (!filled) {
            // Dropping a loan alone does not cancel it. Close this Publisher explicitly;
            // the user can recreate handles with reopen after seeing the failure.
            publisher_->close();
            publisher_.reset();
            return filled;
        }

        auto committed = publisher_->commit(*loan);
        if (!committed) {
            publisher_->close();
            publisher_.reset();
            return committed;
        }

        // Neither loan nor its field proxies may be dereferenced after commit.
        return {};
    }

    static vinc::Result<void> invalid_command() {
        return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                           vinc::module_system::ErrorOperation::CommandHook});
    }

    vinc::Result<void> execute(const std::vector<std::string>& args) {
        if (args.empty() || args[0] == "help") {
            std::cout << "help | status | push <sequence> | loan <sequence> | last | topics | storage\n"
                      << "capacity | params | set <full-or-own-name> <double> | logs | reopen\n";
            return {};
        }

        const auto& command = args[0];
        if ((command == "push" || command == "loan") && args.size() == 2) {
            std::uint64_t sequence = 0;
            auto parsed = std::from_chars(args[1].data(), args[1].data() + args[1].size(), sequence);
            if (parsed.ec != std::errc{} || parsed.ptr != args[1].data() + args[1].size()) {
                return invalid_command();
            }

            return publish(sequence, command == "loan");
        }

        if (command == "storage") {
            return user_example::demonstrate_storage();
        }

        if (command == "last" && latest_) {
            auto message = latest_->get_last();
            if (!message) {
                return std::unexpected(message.error());
            }

            user_example::describe(**message);
            return {};
        }

        if (command == "status") {
            std::cout << "STATUS " << get_module_name() << " mid=" << get_mid()
                      << " living=" << context.living() << " gain=" << gain_ << '\n';
            if (publisher_) {
                auto stats = publisher_->stats();
                std::cout << "PUB published=" << stats.published << " slots=" << stats.effective_slots
                          << " shared_bytes=" << stats.shared_bytes << " collisions=" << stats.collisions
                          << " total_wait_ns=" << stats.total_wait_ns
                          << " longest_wait_ns=" << stats.longest_wait_ns
                          << " current_wait_ns=" << stats.current_wait_ns << '\n';
            }

            if (subscriber_) {
                auto stats = subscriber_->stats();
                std::cout << "SUB lost=" << stats.lost_messages << " overwrite_events=" << stats.overwrite_events
                          << " oversized=" << stats.oversized_messages << '\n';
                for (const auto& source : stats.sources) {
                    std::cout << "SOURCE publisher_id=" << source.publisher << " lost=" << source.lost_messages << '\n';
                }
            }

            return {};
        }

        if (command == "topics") {
            std::string after;
            do {
                auto page = context.list_topics(after);
                if (!page) {
                    return std::unexpected(page.error());
                }

                for (const auto& topic : page->topics) {
                    std::cout << "TOPIC " << topic.name << " schema=" << topic.schema
                              << " publishers=" << topic.publishers << '\n';
                }
                after = page->next;
            } while (!after.empty());
            return {};
        }

        if (command == "params") {
            double cached = 0;
            auto updated = context.param.update("gain", cached);
            if (!updated) {
                return updated;
            }

            std::cout << "CACHE gain=" << cached << '\n';
            for (const auto& watch : context.param.watches()) {
                std::cout << "WATCH " << watch.name << " present=" << watch.present
                          << " compatible=" << watch.compatible << '\n';
            }
            return {};
        }

        if (command == "set" && args.size() == 3) {
            // parse_value is useful when a textual command must preserve an explicit type.
            auto value = vinc::param::parse_value(vinc::param::Type::Double, args[2]);
            if (!value) {
                return std::unexpected(value.error());
            }

            if (args[1] == "gain") {
                return context.param.set<double>(args[1], std::get<double>(*value));
            }

            return context.param.set_value(args[1], std::move(*value));
        }

        if (command == "logs") {
            // Log rejection is nonfatal in this example; do not recursively log the failure.
            for (auto level : {vinc::log::Level::Error, vinc::log::Level::Warn,
                               vinc::log::Level::Info, vinc::log::Level::Debug}) {
                auto written = context.log.write(level, "user example level demonstration");
                if (!written) {
                    report(written.error());
                }
            }

            auto info = context.log.info("Module {} has MID {}", get_module_name(), get_mid());
            if (!info) {
                report(info.error());
            }

            if (context.log.enabled(vinc::log::Level::Debug)) {
                auto debug = context.log.debug("Cached gain = {}", gain_);
                if (!debug) {
                    report(debug.error());
                }
            }

            auto warning = context.log.warn("Demonstration warning; gain={}", gain_);
            if (!warning) {
                report(warning.error());
            }

            auto error = context.log.error("Demonstration error level; the Module stays alive");
            if (!error) {
                report(error.error());
            }

            auto formatted = context.log.format(vinc::log::Level::Info, "Topic {}", topic_);
            if (!formatted) {
                report(formatted.error());
            }

            std::printf("RAW printf from userExampleModule\n");
            std::fflush(stdout);
            return {};
        }

        if (command == "capacity" && publisher_) {
            auto oversized = vinc::make_message<Message>();
            auto resized = oversized->data().note.resize(message_limit_bytes);
            if (!resized) {
                return resized;
            }

            // The text alone fills the budget, so layout overhead makes the image too large.
            auto pushed = publisher_->push(*oversized);
            if (!pushed && pushed.error().is(vinc::message_bus::ErrorCode::CapacityExceeded)) {
                std::cout << "EXPECTED capacity: " << vinc::error_to_string(pushed.error()) << '\n';
                return {};
            }

            if (!pushed) {
                return pushed;
            }

            return invalid_command();
        }

        if (command == "reopen") {
            close_bus();
            auto publisher = open_publisher();
            if (!publisher) {
                return publisher;
            }

            return open_subscribers();
        }

        return invalid_command();
    }
};

REGISTER_MODULE(UserExampleModule, "userExampleModule")
