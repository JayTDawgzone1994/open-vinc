// Interactive example of copied local publication, direct shared-memory loans,
// and copied subscription reads. All operations are invoked through ordinary module hooks.

#include <vinc/module/base_module.hpp>
#include <vinc_messages/test.hpp>
#include <charconv>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
constexpr std::uint32_t example_slots = 2;
constexpr std::uint64_t example_message_bytes = 1024 * 1024;
}

class MessageBusExample final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    using Message = vinc::test;
    std::optional<vinc::message_bus::Publisher<Message>> publisher_;
    std::optional<vinc::message_bus::Subscriber<Message>> subscriber_;
    std::optional<vinc::message_bus::WriteLoan<Message>> loan_;

    static int report(const vinc::Error& failure) {
        std::cout << vinc::error_to_string(failure) << '\n';
        return 1;
    }

    static vinc::Result<void> fill(Message& message, std::int32_t value) {
        message.data().x = true;
        return message.data().y.copy_from(std::vector<std::int32_t>{value});
    }

public:
    using BaseModule::BaseModule;

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == "--name") {
                if (it + 1 == args.end()) {
                    return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                                       vinc::module_system::ErrorOperation::NameHook});
                }

                auto name = *(it + 1);
                args.erase(it, it + 2);
                return name;
            }
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>& input) override {
        auto args = input;
        // Shell includes the command target; direct ModuleManager callers may omit it.
        if (!args.empty() && args.front() == "messageBusExample") {
            args.erase(args.begin());
        }

        if (args.empty() || (args[0] != "pub" && args[0] != "sub")) {
            std::cout << "Usage: messageBusExample pub|sub [topic]\n";
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }

        const auto topic = args.size() > 1 ? args[1] : "demo";
        if (args[0] == "pub") {
            auto publisher = context.advertise<Message>(
                topic, {.slots = example_slots, .max_message_bytes = example_message_bytes});
            if (!publisher) {
                return std::unexpected(publisher.error());
            }

            publisher_.emplace(std::move(*publisher));
        } else {
            auto subscription = context.subscribe<Message>(topic, [this](std::unique_ptr<Message> message) {
                std::cout << '[' << get_module_name() << "] callback count="
                          << message->get_topic_count() << '\n';
            });
            if (!subscription) {
                return std::unexpected(subscription.error());
            }

            subscriber_.emplace(std::move(*subscription));
        }

        std::cout << '[' << get_module_name() << "] Message Bus " << args[0] << " topic=" << topic << '\n';
        return {};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        if (args.empty()) {
            return 1;
        }

        const auto& command = args[0];
        if (subscriber_ && command == "last") {
            auto result = subscriber_->get_last();
            if (!result) {
                return report(result.error());
            }
            // The framework has already copied the payload locally and released its read lease.
            // Application processing here cannot keep the Publisher waiting on this message.
            auto& message = **result;
            std::cout << '[' << get_module_name() << "] value=" << message.data().y.at(0).value()
                      << " publisher=" << message.publisher_name() << " count=" << message.get_topic_count()
                      << " timestamp_ns=" << message.get_message_timestamp() << '\n';
            return 0;
        }

        if (!publisher_) {
            return 1;
        }

        if (command == "stats") {
            auto snapshot = publisher_->stats();
            std::cout << "slots=" << snapshot.effective_slots << " published=" << snapshot.published
                      << " collisions=" << snapshot.collisions << " total_wait_ns=" << snapshot.total_wait_ns
                      << " longest_wait_ns=" << snapshot.longest_wait_ns
                      << " current_wait_ns=" << snapshot.current_wait_ns << '\n';
            return 0;
        }

        if (command == "commit") {
            if (!loan_) {
                std::cout << "No outstanding loan\n";
                return 1;
            }

            auto result = publisher_->commit(*loan_);
            if (!result) {
                return report(result.error());
            }

            loan_.reset();
            return 0;
        }

        if ((command == "push" || command == "loan") && args.size() == 2) {
            std::int32_t value = 0;
            auto [end, parse_error] = std::from_chars(args[1].data(), args[1].data() + args[1].size(), value);
            if (parse_error != std::errc{} || end != args[1].data() + args[1].size()) {
                return 1;
            }
            // Path 1: build freely in local memory; push copies the packed payload into
            // the Publisher ring buffer and fails if it exceeds this Publisher's byte budget.
            if (command == "push") {
                auto message = vinc::make_message<Message>();
                auto filled = fill(*message, value);
                if (!filled) {
                    return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                                                            vinc::module_system::ErrorOperation::CommandHook,
                                                            filled.error()));
                }
                auto result = publisher_->push(*message);
                return result ? 0 : report(result.error());
            }
            // Path 2: borrow a fixed shared slot now and publish later with commit.
            // Array growth can fail immediately here because the shared byte budget already applies.
            auto result = publisher_->get_next();
            if (!result) {
                return report(result.error());
            }

            loan_.emplace(std::move(*result));
            auto filled = fill(**loan_, value);
            if (!filled) {
                return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                                                        vinc::module_system::ErrorOperation::CommandHook,
                                                        filled.error()));
            }
            return 0;
        }

        return 1;
    }

    vinc::Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) override {
        return publisher_ ? std::vector<std::string>{"push", "loan", "commit", "stats"}
                          : std::vector<std::string>{"last"};
    }
};
REGISTER_MODULE(MessageBusExample, "messageBusExample")
