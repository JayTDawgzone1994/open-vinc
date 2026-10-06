#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto application_poll_interval = 10ms;
constexpr auto deliberately_stalled_callback = 10s;

void require(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template <class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

class ContextModule : public BaseModule<RunMode::Manual> {
    std::string mode_;
    std::thread::id main_thread_;
    std::jthread worker_;
    std::atomic<bool> pump_started_{false};
    std::optional<message_bus::Publisher<Frame>> publisher_;
    std::optional<message_bus::Subscriber<Frame>> subscriber_;
    std::unique_ptr<Frame> saved_;
    std::vector<std::uint64_t> received_;

    void publish(std::uint64_t value) {
        auto message = make_message<Frame>();
        message->data().sequence = value;
        take(publisher_->push(*message));
    }

    void create_bus(std::uint32_t slots = 8) {
        publisher_.emplace(take(context.advertise<Frame>(get_module_name(), {.slots = slots})));
    }

    void verify_batch() {
        create_bus();
        subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [this](std::unique_ptr<Frame> message) {
            received_.push_back(message->data().sequence.get());
            saved_ = std::move(message);
            if (received_.size() == 1) {
                publish(3);
                auto recursive = context.pumpOnce();
                require(!recursive && recursive.error().is(module_system::ErrorCode::Busy), "recursive pump rejected");
                require(take(subscriber_->get_last())->data().sequence.get() == 3, "callback can read latest");
            }
        })));
        publish(1);
        publish(2);
        auto first = take(context.pumpOnce());
        require(first.callbacks == 2 && received_ == std::vector<std::uint64_t>{1, 2}, "batch freezes upper boundary");
        require(take(subscriber_->get_last())->data().sequence.get() == 3, "latest leaves callback cursor");
        require(take(context.pumpOnce()).callbacks == 1 && received_.back() == 3, "next batch includes new commit");
        require(take(context.pumpOnce()).callbacks == 0, "empty batch succeeds");
        require(saved_->data().sequence.get() == 3, "callback transfers owned message");
    }

    void verify_loss() {
        create_bus(2);
        subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [this](std::unique_ptr<Frame> message) {
            received_.push_back(message->data().sequence.get());
        })));
        publish(1);
        publish(2);
        publish(3);
        // Borrowing evicts sequence 2 before publishing sequence 4.
        auto loan = take(publisher_->get_next());
        auto batch = take(context.pumpOnce());
        require(received_ == std::vector<std::uint64_t>{3}, "copy only retained publication");
        require(batch.lost_messages == 2 && subscriber_->stats().lost_messages == 2, "count pre-commit eviction");
        require(take(context.pumpOnce()).lost_messages == 0, "do not recount losses");
    }

    void verify_cancel() {
        create_bus();
        subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [this](std::unique_ptr<Frame>) {
            subscriber_->close();
        })));
        publish(1);
        publish(2);
        require(take(context.pumpOnce()).callbacks == 1, "self-close suppresses remaining batch");
        require(take(context.pumpOnce()).callbacks == 0, "closed callback removed");
    }

    void verify_topology() {
        create_bus();
        auto second = take(context.advertise<Frame>(get_module_name()));
        std::optional<message_bus::Publisher<Frame>> added_publisher;
        std::optional<message_bus::Subscriber<Frame>> added_subscriber;
        std::uint64_t added_callbacks = 0;
        subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [&](std::unique_ptr<Frame> message) {
            received_.push_back(message->data().sequence.get());
            if (received_.size() == 1) {
                added_publisher.emplace(take(context.advertise<Frame>(get_module_name())));
                auto added = make_message<Frame>();
                added->data().sequence = 3;
                take(added_publisher->push(*added));
                added_subscriber.emplace(take(context.subscribe<Frame>(get_module_name(),
                    [&](std::unique_ptr<Frame>) {
                        ++added_callbacks;
                    })));
            }
        })));
        publish(1);
        auto message = make_message<Frame>();
        message->data().sequence = 2;
        take(second.push(*message));
        require(take(context.pumpOnce()).callbacks == 2, "capture both initial publishers");
        require(received_ == std::vector<std::uint64_t>{1, 2}, "timestamp merge preserves publication order");
        require(added_callbacks == 0, "new subscription waits for next pump");
        require(take(context.pumpOnce()).callbacks == 4, "new source and subscription participate next pump");
        require(received_.back() == 3 && added_callbacks == 3, "topology changes do not extend active batch");
        subscriber_->close();
    }

    void verify_new_only() {
        create_bus();
        publish(1);
        publish(2);
        subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [this](std::unique_ptr<Frame> message) {
            received_.push_back(message->data().sequence.get());
        }, {.start = message_bus::StartPosition::NewOnly})));
        require(take(context.pumpOnce()).callbacks == 0, "NewOnly excludes old data");
        require(subscriber_->stats().lost_messages == 0, "intentional history skip is not loss");
        publish(3);
        require(take(context.pumpOnce()).callbacks == 1 && received_.back() == 3, "NewOnly receives later commit");
    }

public:
    using BaseModule::BaseModule;
    Result<void> on_start(const std::vector<std::string>& args) override {
        main_thread_ = std::this_thread::get_id();
        mode_ = args.at(0);
        return {};
    }

    Result<void> main() override {
        if (mode_ == "batch") {
            verify_batch();
        } else if (mode_ == "loss") {
            verify_loss();
        } else if (mode_ == "cancel") {
            verify_cancel();
        } else if (mode_ == "topology") {
            verify_topology();
        } else if (mode_ == "new-only") {
            verify_new_only();
        } else if (mode_ == "error") {
            return std::unexpected(Error{module_system::ErrorCode::InvalidArgument, module_system::ErrorOperation::MainHook});
        } else if (mode_ == "exit-zero") {
            std::_Exit(0);
        } else if (mode_ == "callback-error" || mode_ == "callback-stall") {
            create_bus();
            subscriber_.emplace(take(context.subscribe<Frame>(get_module_name(), [this](std::unique_ptr<Frame>) {
                if (mode_ == "callback-stall") {
                    std::this_thread::sleep_for(deliberately_stalled_callback);
                } else {
                    throw std::runtime_error("unhandled user callback");
                }
            })));
            publish(1);
            (void)context.pumpOnce();
            // Deliberately ignore the Result and living(): Host supervision must still exit.
            for (;;) {
                std::this_thread::sleep_for(application_poll_interval);
            }
        } else if (mode_ == "worker") {
            worker_ = std::jthread([this] {
                pump_started_.store(true);
                take(context.pumpLoop());
            });
            while (context.living()) {
                std::this_thread::sleep_for(application_poll_interval);
            }
        } else if (mode_ == "loop") {
            return context.pumpLoop();
        }

        return {};
    }

    Result<void> on_stop() override {
        if (worker_.joinable()) {
            worker_.join();
        }

        if (mode_ == "cleanup-error") {
            return std::unexpected(Error{module_system::ErrorCode::CleanupFailed, module_system::ErrorOperation::StopHook});
        }

        if (mode_ == "cleanup-stall") {
            std::this_thread::sleep_for(deliberately_stalled_callback);
        }

        return {};
    }

    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        if (mode_ == "worker" && std::this_thread::get_id() == main_thread_) {
            return -1;
        }

        if (mode_ == "loop" && std::this_thread::get_id() != main_thread_) {
            return -1;
        }

        return 42;
    }
};
static_assert(!std::is_default_constructible_v<Context>);
static_assert(!std::is_copy_constructible_v<Context>);
static_assert(std::is_abstract_v<BaseModule<RunMode::Manual>>);
}

REGISTER_MODULE(ContextModule, "contextTest")
