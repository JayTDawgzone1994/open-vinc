#include <vinc/vinc.hpp>
#include <vinc_messages/SdkPublisherSample.hpp>
#include <charconv>
#include <optional>

class PublisherDefaultsProbe : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    using Message = vinc::SdkPublisherSample;
    std::optional<vinc::message_bus::Publisher<Message>> original_;

public:
    PublisherDefaultsProbe() : BaseModule("probe") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        auto publisher = context.advertise<Message>("sdk/defaults/original");
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        original_.emplace(std::move(*publisher));
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }
        if (args.size() != 3) {
            return 1;
        }

        std::uint32_t slots = 0;
        std::uint64_t bytes = 0;
        const auto parsed_slots = std::from_chars(args[1].data(), args[1].data() + args[1].size(), slots);
        const auto parsed_bytes = std::from_chars(args[2].data(), args[2].data() + args[2].size(), bytes);
        if (parsed_slots.ec != std::errc{} || parsed_slots.ptr != args[1].data() + args[1].size() ||
            parsed_bytes.ec != std::errc{} || parsed_bytes.ptr != args[2].data() + args[2].size()) {
            return 1;
        }

        if (args[0] == "original") {
            const auto stats = original_->stats();
            return stats.requested_slots == slots && stats.max_message_bytes == bytes ? 0 : 1;
        }
        if (args[0] != "current") {
            return 1;
        }

        auto publisher = context.advertise<Message>("sdk/defaults/current");
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        const auto stats = publisher->stats();
        auto committed = context.param.read_current("vinc.pub_default_slots");
        if (!committed) {
            return std::unexpected(committed.error());
        }

        auto current = committed->get<std::uint32_t>();
        if (!current) {
            return std::unexpected(current.error());
        }

        if (stats.requested_slots != slots || stats.max_message_bytes != bytes || *current != slots) {
            return 1;
        }

        auto message = vinc::make_message<Message>();
        message->data().value = 42;
        auto pushed = publisher->push(*message);
        if (!pushed) {
            return std::unexpected(pushed.error());
        }

        return publisher->stats().published == 1 ? 0 : 1;
    }
};

REGISTER_MODULE(PublisherDefaultsProbe, "PublisherDefaultsProbe")
