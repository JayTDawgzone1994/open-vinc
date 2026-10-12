// Message Bus fault-injection module used only by tests. Private ring buffer access deliberately
// holds a reader lease so confirmed process death can exercise BusManager reclamation.

#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc_messages/Point.hpp>
#include <cstdlib>
#include <optional>
#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <vinc/message_bus/detail/runtime.hpp>

namespace {
constexpr int helper_error_base = -100;
constexpr unsigned payload_pattern_modulus = 251;
constexpr std::size_t default_payload_bytes = 16;
}

class MessageBusTestModule : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::optional<vinc::message_bus::Publisher<vinc::Frame>> publisher_;
    std::optional<vinc::message_bus::Subscriber<vinc::Frame>> subscriber_;
    std::optional<vinc::message_bus::WriteLoan<vinc::Frame>> loan_;
    std::unique_ptr<vinc::Frame> copied_;
    std::string topic_;
    std::shared_ptr<vinc::message_bus::detail::RingBuffer> held_ring_buffer_;
    std::uint32_t held_reader_ = 0;

    vinc::Result<std::unique_ptr<vinc::Frame>> next_for_test() {
        auto core = vinc::message_bus::detail::DispatchAccess::core(*subscriber_);
        auto boundary = vinc::message_bus::detail::capture(core);
        if (!boundary) {
            return std::unexpected(boundary.error());
        }
        auto result = vinc::message_bus::detail::read_batch(core, *boundary, vinc::Frame::schema(),
            [](vinc::message::detail::Binding binding) -> std::unique_ptr<vinc::BaseMessage> {
                return std::unique_ptr<vinc::Frame>(new vinc::Frame(
                    vinc::message::detail::RecordAccess::bind<vinc::Frame>(std::move(binding))));
            });
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::unique_ptr<vinc::Frame>(static_cast<vinc::Frame*>(result->release()));
    }

    static int failure(const vinc::Error& failure) {
        if (failure.domain == vinc::ErrorDomain::ModuleSystem && !failure.causes().empty()) {
            const auto& cause = failure.causes().front();
            if (cause.domain == vinc::ErrorDomain::MessageBus) {
                return helper_error_base - cause.code;
            }
        }
        return helper_error_base - static_cast<int>(failure.code);
    }

    static vinc::Result<void> fill(vinc::Frame& frame, std::uint64_t value, std::size_t bytes) {
        frame.data().sequence = value;
        auto payload = frame.data().payload.copy_from(
            std::vector<std::uint8_t>(bytes, static_cast<std::uint8_t>(value % payload_pattern_modulus)));
        if (!payload) {
            return payload;
        }
        auto resized = frame.data().detections.resize(1);
        if (!resized) {
            return resized;
        }
        frame.data().detections[0].data().id = static_cast<std::uint32_t>(value);
        return frame.data().detections[0].data().label.assign("nested detection");
    }

public:
    explicit MessageBusTestModule(std::string name) : BaseModule(std::move(name)) {
    }

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        if (args.size() >= 2 && args[0] == "--name") {
            auto name = args[1];
            args.erase(args.begin(), args.begin() + 2);
            return name;
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (args.size() < 2) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }
        topic_ = args[1];
        if (args[0] == "pub") {
            vinc::message_bus::PublisherOptions options;
            if (args.size() > 2) {
                options.slots = static_cast<std::uint32_t>(std::stoul(args[2]));
            }

            if (args.size() > 3) {
                options.max_message_bytes = std::stoull(args[3]);
            }

            auto result = context.advertise<vinc::Frame>(topic_, options);
            if (!result) {
                return std::unexpected(result.error());
            }

            publisher_.emplace(std::move(*result));
        } else {
            vinc::message_bus::SubscriberOptions options;
            if (args.size() > 2 && args[2] == "new") {
                options.start = vinc::message_bus::StartPosition::NewOnly;
            }

            auto result = context.subscribe<vinc::Frame>(topic_, options);
            if (!result) {
                return std::unexpected(result.error());
            }

            subscriber_.emplace(std::move(*result));
        }

        return {};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string> args) override {
        if (args.empty()) {
            return 0;
        }

        const auto& command = args[0];
        if (command == "crash") {
            std::_Exit(42);
        }

        if (command == "pin") {
            auto attachments = vinc::message_bus::detail::inspect_sources(vinc::message_bus::detail::DispatchAccess::core(*subscriber_));
            if (attachments.size() != 1) {
                return -10;
            }

            auto& attachment = attachments[0];
            auto ring_buffer = vinc::message_bus::detail::RingBuffer::open(attachment.descriptor, false);
            if (!ring_buffer) {
                return failure(ring_buffer.error());
            }

            auto candidate = (*ring_buffer)->candidate(0, true);
            if (!candidate || !(*ring_buffer)->protect(*candidate, attachment.reader)) {
                return -11;
            }
            held_ring_buffer_ = std::move(*ring_buffer);
            held_reader_ = attachment.reader;
            return 0; // Test-only pause at the protected copy boundary.
        }

        if (command == "unpin") {
            if (held_ring_buffer_) {
                held_ring_buffer_->unprotect(held_reader_);
            }

            held_ring_buffer_.reset();
            return 0;
        }

        if (command == "wrong-schema") {
            auto result = context.advertise<vinc::Point>(topic_);
            return result ? 999 : failure(result.error());
        }

        if (command == "next" || command == "last") {
            auto result = command == "next" ? next_for_test() : subscriber_->get_last();
            if (!result) {
                return failure(result.error());
            }
            copied_ = std::move(*result);
            if (copied_->topic_name() != topic_ || copied_->publisher_name() == "unknown" ||
                !copied_->get_message_timestamp() || !copied_->get_topic_count() ||
                !copied_->publisher_instance()) {
                return -999;
            }

            const auto value = copied_->data().sequence.get();
            if (copied_->data().detections[0].data().id.get() != value ||
                copied_->data().detections[0].data().label.str() != "nested detection") {
                return -998;
            }

            for (const auto byte : std::as_const(copied_->data().payload)) {
                if (byte != value % payload_pattern_modulus) {
                    return -997;
                }
            }

            return static_cast<int32_t>(value);
        }

        if (command == "copy-value") {
            return copied_ ? static_cast<int32_t>(copied_->data().sequence.get()) : -1;
        }

        if (command == "publisher-is" && args.size() == 2) {
            return copied_ && copied_->publisher_name() == args[1] ? 1 : 0;
        }

        if (command == "copy-size") {
            return copied_ ? static_cast<int32_t>(copied_->data().payload.size()) : -1;
        }

        if (command == "count") {
            return copied_ ? static_cast<int32_t>(copied_->get_topic_count()) : -1;
        }

        if (command == "slots") {
            return static_cast<int32_t>(publisher_->stats().effective_slots);
        }

        if (command == "published") {
            return static_cast<int32_t>(publisher_->stats().published);
        }

        if (command == "collisions") {
            return static_cast<int32_t>(publisher_->stats().collisions);
        }

        if (command == "waited") {
            return publisher_->stats().total_wait_ns > 0 && publisher_->stats().longest_wait_ns > 0 ? 1 : 0;
        }

        if (command == "drop") {
            loan_.reset();
            return 0;
        }

        if (command == "commit") {
            if (!loan_) {
                return -1;
            }

            auto result = publisher_->commit(*loan_);
            if (!result) {
                return failure(result.error());
            }

            loan_.reset();
            return 0;
        }

        if (command == "close") {
            publisher_.reset();
            return 0;
        }

        if (command == "push" || command == "loan") {
            const auto value = std::stoull(args.at(1));
            const auto bytes = args.size() > 2 ? std::stoull(args[2]) : default_payload_bytes;
            if (command == "loan") {
                auto result = publisher_->get_next();
                if (!result) {
                    return failure(result.error());
                }

                loan_.emplace(std::move(*result));
                auto filled = fill(**loan_, value, static_cast<std::size_t>(bytes));
                if (!filled) {
                    return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                                                            vinc::module_system::ErrorOperation::CommandHook,
                                                            filled.error()));
                }
                return 0;
            }

            auto message = vinc::make_message<vinc::Frame>();
            auto filled = fill(*message, value, static_cast<std::size_t>(bytes));
            if (!filled) {
                return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                                                        vinc::module_system::ErrorOperation::CommandHook,
                                                        filled.error()));
            }
            auto result = publisher_->push(*message);
            if (message->data().sequence.get() != value || message->data().payload.size() != bytes) {
                return -996;
            }

            return result ? 0 : failure(result.error());
        }

        return -2;
    }
};
REGISTER_MODULE(MessageBusTestModule, "messageBusTest")
