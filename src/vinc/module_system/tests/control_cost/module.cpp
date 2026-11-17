#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>

class ControlCost : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::vector<vinc::message_bus::Subscriber<vinc::Frame>> subscribers_;

    vinc::Result<void> add() {
        auto subscriber = context.subscribe<vinc::Frame>(
            "control-cost/topic" + std::to_string(subscribers_.size()),
            [](std::unique_ptr<vinc::Frame>) {});
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }
        subscribers_.push_back(std::move(*subscriber));
        return {};
    }

public:
    ControlCost() : BaseModule("controlCostNode") {}

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        const auto count = std::stoul(args.at(0));
        subscribers_.reserve(2 * count);
        for (std::size_t index = 0; index < count; ++index) {
            auto result = add();
            if (!result) {
                return result;
            }
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        auto result = add();
        if (!result) {
            return std::unexpected(result.error());
        }
        return 0;
    }
};
REGISTER_MODULE(ControlCost, "controlCost")
