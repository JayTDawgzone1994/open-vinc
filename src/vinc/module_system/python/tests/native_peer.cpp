#include <vinc/module/base_module.hpp>
#include <vinc_messages/int3.hpp>
#include <optional>
#include <limits>

namespace {
using namespace vinc;
class NativePeer : public BaseModule<RunMode::FrameworkLoop> {
    std::optional<message_bus::Publisher<int3>> padding_publisher_;
    std::optional<message_bus::Publisher<int3>> publisher_;
    std::optional<message_bus::Subscriber<int3>> subscriber_;
public:
    NativePeer() : BaseModule("nativeNode") {
    }
    Result<void> on_start(const std::vector<std::string>& args) override {
        auto limited = context.param.register_param<std::int32_t>("fixed", 7,
            {.restart_hint = param::RestartHint::Module});
        if (!limited) {
            return limited;
        }

        auto shared = context.param.register_param<std::int32_t>("shared", 7, {.permission = param::Permission::Public});
        if (!shared) {
            return shared;
        }

        // Consume one Bus ID without allocating another MID. The reply Publisher
        // can then share its numeric ID with the receiving Python Module.
        if (!args.empty() && args.front() == "same-id") {
            auto padding = context.advertise<int3>("python/padding");
            if (!padding) {
                return std::unexpected(padding.error());
            }

            padding_publisher_.emplace(std::move(*padding));
        }

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
    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        // Test-only query: obtain the expected identity independently of received
        // message metadata. This fixture uses only a few IDs, fitting the hook result.
        if (!args.empty() && args.front() == "publisher-id") {
            std::string cursor;
            do {
                auto diagnostics = context.inspect_topics(cursor);
                if (!diagnostics) {
                    return std::unexpected(diagnostics.error());
                }

                for (const auto& topic : diagnostics->topics) {
                    if (topic.name != "python/reply") {
                        continue;
                    }

                    for (const auto& publisher : topic.publishers) {
                        if (publisher.owner == get_mid() && publisher.name == "nativeNode") {
                            if (publisher.id == 0 || publisher.id > (std::numeric_limits<std::int32_t>::max)()) {
                                contract_violation("Fixture Publisher ID does not fit the test hook result");
                            }

                            return static_cast<std::int32_t>(publisher.id);
                        }
                    }
                }

                cursor = diagnostics->next;
            } while (!cursor.empty());

            contract_violation("Fixture reply Publisher missing from Bus diagnostics");
        }

        return 0;
    }
};
}
REGISTER_MODULE(NativePeer, "nativePeer")
