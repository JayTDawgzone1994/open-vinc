// Small production-message publisher for real UI/ModuleHost integration tests.
#include <vinc/module/base_module.hpp>
#include <vinc_messages/UserExampleFrame.hpp>
#include <optional>

class TopicSource final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::optional<vinc::message_bus::Publisher<vinc::UserExampleFrame>> publisher_;

public:
    TopicSource() : BaseModule("previewSource") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        auto publisher = context.advertise<vinc::UserExampleFrame>("ui/preview", {.slots = 4, .max_message_bytes = 4096});
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publisher_.emplace(std::move(*publisher));
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        auto message = vinc::make_message<vinc::UserExampleFrame>();
        message->data().sequence = 42;
        auto note = message->data().note.assign("<script>plain text</script>");
        if (!note) {
            return std::unexpected(note.error());
        }

        auto published = publisher_->push(*message);
        if (!published) {
            return std::unexpected(published.error());
        }

        return 0;
    }
};
REGISTER_MODULE(TopicSource, "uiTopicSource")
