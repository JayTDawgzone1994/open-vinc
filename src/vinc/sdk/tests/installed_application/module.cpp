#include <vinc/vinc.hpp>
#include <vinc_messages/AppFrame.hpp>
#include <iostream>
#include <optional>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

class ApplicationModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    using Message = vinc::AppFrame;
    std::optional<vinc::message_bus::Publisher<Message>> publisher_;
    std::optional<vinc::message_bus::Subscriber<Message>> subscriber_;
    bool self_path_verified_ = false;

public:
#ifdef TEST_SENDER
    ApplicationModule() : BaseModule("sender") {
    }
#else
    ApplicationModule() : BaseModule("reader") {
    }
#endif

    vinc::Result<void> on_start(const std::vector<std::string>& arguments) override {
        // The relocation flow supplies the central image path. Minimal feature
        // checks omit it; only the full flow reports self-host identity below.
        if (arguments.size() == 2) {
            auto expected = vinc::osal::path_from_utf8(arguments[1]);
            auto actual = vinc::osal::executable_path();
            if (!expected || !actual || *expected != *actual) {
                return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                    vinc::module_system::ErrorOperation::StartHook});
            }

            self_path_verified_ = true;
        }

#ifdef TEST_SENDER
        constexpr std::uint64_t message_bytes = 4096;
        constexpr std::uint32_t retained_messages = 8;
        auto publisher = context.advertise<Message>("installed/custom",
            {.slots = retained_messages, .max_message_bytes = message_bytes});
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publisher_.emplace(std::move(*publisher));
#else
        auto subscriber = context.subscribe<Message>("installed/custom",
            [](std::unique_ptr<Message> message) {
                std::cout << "CALLBACK=" << message->data().leaf.data().value << '\n' << std::flush;
            });
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscriber_.emplace(std::move(*subscriber));
#endif
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (args.size() == 2 && args[1] == "identity") {
            if (!self_path_verified_) {
                return 1;
            }

#ifdef _WIN32
            const auto pid = _getpid();
#else
            const auto pid = getpid();
#endif
            std::cout << "PACKAGE_HOST_PID=" << pid << '\n' << std::flush;
            return 0;
        }

        if (publisher_) {
            if (args.size() == 2 && args[1] == "loan") {
                auto loan = publisher_->get_next();
                if (!loan) {
                    return std::unexpected(loan.error());
                }

                (*loan)->data().leaf.data().value = vinc::AppLeaf::revision;
                auto committed = publisher_->commit(*loan);
                if (!committed) {
                    return std::unexpected(committed.error());
                }

                return 0;
            }

            auto message = vinc::make_message<Message>();
            message->data().leaf.data().value = vinc::AppLeaf::revision;
            auto sent = publisher_->push(*message);
            if (!sent) {
                return std::unexpected(sent.error());
            }

            return 0;
        }

        auto message = subscriber_->get_last();
        if (!message) {
            return 1;
        }

        const auto value = (*message)->data().leaf.data().value;
        std::cout << "RECEIVED=" << value << " publisher=" << (*message)->publisher_name() << '\n'
                  << std::flush;
        return value == vinc::AppLeaf::revision ? 0 : 1;
    }
};

#ifdef TEST_SENDER
REGISTER_MODULE(ApplicationModule, "AppSender")
#else
REGISTER_MODULE(ApplicationModule, "AppReader")
#endif
