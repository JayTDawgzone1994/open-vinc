#include <vinc/module/base_module.hpp>
#include <vinc_messages/UserExampleFrame.hpp>

class GraphSource final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::vector<vinc::message_bus::Publisher<vinc::UserExampleFrame>> publishers_;
    std::vector<vinc::message_bus::Subscriber<vinc::UserExampleFrame>> subscribers_;
public:
    GraphSource() : BaseModule("graphSource") {
    }
    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        return args.empty() ? get_module_name() : args.front();
    }
    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (args.size() > 1 && args[1] == "empty") {
            return {};
        }

        constexpr std::uint32_t fixture_slots = 2;
        constexpr std::uint64_t fixture_message_bytes = 4096;
        constexpr std::size_t publishers = 2;
        constexpr std::size_t subscribers = 40; // Deliberately spans a 32-row page within one topic.
        for (std::size_t index = 0; index < publishers; ++index) {
            auto publisher = context.advertise<vinc::UserExampleFrame>("graph/shared", {.slots = fixture_slots, .max_message_bytes = fixture_message_bytes});
            if (!publisher) {
                return std::unexpected(publisher.error());
            }
            publishers_.push_back(std::move(*publisher));
        }
        for (std::size_t index = 0; index < subscribers + 1; ++index) {
            auto subscriber = context.subscribe<vinc::UserExampleFrame>(
                index == subscribers ? "graph/noPublisher" : "graph/shared");
            if (!subscriber) {
                return std::unexpected(subscriber.error());
            }
            subscribers_.push_back(std::move(*subscriber));
        }
        return {};
    }
};
REGISTER_MODULE(GraphSource, "uiGraphSource")
