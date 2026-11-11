#include <vinc/vinc.hpp>
#include <vinc/bag/bag.hpp>
#include <vinc_messages/RecorderSample.hpp>
#include <optional>

namespace {
constexpr std::uint64_t record_count = 64;
constexpr std::uint32_t ring_slots = 128;
constexpr std::uint64_t message_bytes = 4096;
constexpr std::string_view topic_name = "sdk/recorder";
}

class RecorderProbe final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::optional<vinc::message_bus::Publisher<vinc::RecorderSample>> publisher_;
public:
    RecorderProbe() : BaseModule("probe") {}
    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        auto publisher = context.advertise<vinc::RecorderSample>(std::string(topic_name),
            {.slots = ring_slots, .max_message_bytes = message_bytes});
        if (!publisher) {
            return std::unexpected(publisher.error());
        }
        publisher_.emplace(std::move(*publisher));
        return {};
    }
    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == "probe") {
            args.erase(args.begin());
        }
        if (args.size() == 1 && args[0] == "ready") {
            auto page = context.inspect_topics();
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const auto& topic : page->topics) {
                if (topic.name == topic_name && topic.subscribers == 1) {
                    return 0;
                }
            }
            return 1;
        }
        if (args.size() == 1 && args[0] == "burst") {
            auto message = vinc::make_message<vinc::RecorderSample>();
            for (std::uint64_t sequence = 1; sequence <= record_count; ++sequence) {
                message->data().sequence = sequence;
                auto published = publisher_->push(*message);
                if (!published) {
                    return std::unexpected(published.error());
                }
            }
            return 0;
        }
        if (args.size() == 2 && (args[0] == "written" || args[0] == "complete")) {
            auto path = vinc::osal::path_from_utf8(args[1]);
            if (!path) {
                return std::unexpected(path.error());
            }
            auto info = vinc::bag::inspect(*path);
            if (!info) {
                return std::unexpected(info.error());
            }
            const bool intact = info->counters.received == record_count &&
                info->counters.written == record_count && info->counters.bus_lost == 0 &&
                info->counters.queue_dropped == 0 && info->counters.oversized == 0;
            return intact && (args[0] == "written" || info->completed) ? 0 : 1;
        }
        return 1;
    }
};
REGISTER_MODULE(RecorderProbe, "RecorderProbe")
