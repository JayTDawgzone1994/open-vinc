#include <vinc/module/base_module.hpp>
#include <vinc_messages/int3.hpp>
#include <optional>

namespace {
using namespace vinc;
class NativePeer : public BaseModule<RunMode::FrameworkLoop> {
    std::optional<message_bus::Publisher<int3>> publisher_;
    std::optional<message_bus::Subscriber<int3>> subscriber_;
public:
    using BaseModule::BaseModule;
    Result<void> on_start(const std::vector<std::string>&) override {
        auto publisher = context.advertise<int3>("python/reply");
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publisher_.emplace(std::move(*publisher));
        auto subscriber = context.subscribe<int3>("python/request", [this](std::unique_ptr<int3> message) {
            if (message->publisher_name() != "pythonNode" || message->data().x.get() != 42 ||
                message->topic_name() != "python/request") {
                contract_violation("Python/native fixture received unexpected data or identity");
            }

            message->data().y = 84;
            auto sent = publisher_->push(*message);
            if (!sent) {
                contract_violation("Python/native fixture failed to echo");
            }
        });
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscriber_.emplace(std::move(*subscriber));
        return {};
    }
    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};
}
REGISTER_MODULE(NativePeer, "nativePeer")
