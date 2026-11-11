#include <vinc/vinc.hpp>
#include <vinc_messages/int3.hpp>
#include <array>
#include <iostream>
#include <mutex>
#include <optional>

class ParallelExample final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    static constexpr std::uint32_t topic_workers = 2;
    static constexpr std::size_t topic_count = 2;
    std::mutex mutex_;
    std::int32_t gain_ = 1;
    std::uint64_t received_ = 0;
    std::int64_t latest_result_ = 0;
    std::array<std::unique_ptr<vinc::int3>, topic_count> latest_;
    std::array<std::optional<vinc::message_bus::Subscriber<vinc::int3>>, topic_count> subscribers_;
    std::array<std::optional<vinc::message_bus::Publisher<vinc::int3>>, topic_count> publishers_;

    void receive(std::size_t index, std::unique_ptr<vinc::int3> message) {
        std::int32_t gain;
        {
            std::lock_guard lock(mutex_);
            gain = gain_;
        }

        // Replace this bounded computation with application work. Do not hold
        // the shared-state lock during I/O, SDK calls or expensive processing.
        const auto result = static_cast<std::int64_t>(message->data().x.get()) * gain;
        {
            std::lock_guard lock(mutex_);
            latest_result_ = result;
            ++received_;
            latest_[index] = std::move(message);
        }
    }

    vinc::Result<void> connect_topic(std::size_t index, const std::string& name) {
        auto publisher = context.advertise<vinc::int3>(name);
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publishers_[index].emplace(std::move(*publisher));
        auto subscriber = context.subscribe<vinc::int3>(name, [this, index](auto message) {
            receive(index, std::move(message));
        });
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscribers_[index].emplace(std::move(*subscriber));
        return {};
    }

    vinc::Result<void> configure_gain() {
        auto registered = context.param.register_param<std::int32_t>("gain", 1);
        if (!registered) {
            return registered;
        }

        return context.param.on_change([this](const vinc::param::ParamMap& values) {
            auto found = values.find(get_module_name() + ".gain");
            if (found == values.end()) {
                return;
            }

            auto gain = found->second.get<std::int32_t>();
            if (!gain) {
                std::cerr << vinc::error_to_string(gain.error()) << '\n';
                return;
            }

            // Retain the scalar, never a reference into the callback's map.
            std::lock_guard lock(mutex_);
            gain_ = *gain;
        });
    }

public:
    ParallelExample() : BaseModule("parallelCpp", {.topic_workers = topic_workers}) {
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        auto configured = configure_gain();
        if (!configured) {
            return configured;
        }

        auto left = connect_topic(0, "parallel/left");
        if (!left) {
            return left;
        }

        return connect_topic(1, "parallel/right");
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.back() == "send") {
            auto message = vinc::make_message<vinc::int3>();
            message->data().x = 42;
            // Only the serial control dispatcher uses these Publisher handles.
            for (auto& publisher : publishers_) {
                auto sent = publisher->push(*message);
                if (!sent) {
                    std::cerr << vinc::error_to_string(sent.error()) << '\n';
                    return 1;
                }
            }
            return 0;
        }

        std::uint64_t count;
        std::int64_t result;
        {
            std::lock_guard lock(mutex_);
            count = received_;
            result = latest_result_;
        }
        std::cout << get_module_name() << " received=" << count << " scaled=" << result << '\n' << std::flush;
        return 0;
    }

    vinc::Result<void> on_stop() override {
        // Framework callbacks have drained; retained messages are still owned
        // here. Application-created threads would need their own stop/join.
        latest_ = {};
        return {};
    }
};

REGISTER_MODULE(ParallelExample, "parallelExample")
