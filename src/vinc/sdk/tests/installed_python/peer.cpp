#include <vinc/vinc.hpp>
#include <vinc_messages/SdkPacket.hpp>
#include <iostream>
#include <optional>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

class NativePeer final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    using Packet = vinc::SdkPacket;
    std::optional<vinc::message_bus::Publisher<Packet>> publisher_;
    std::optional<vinc::message_bus::Subscriber<Packet>> subscriber_;

public:
    NativePeer() : BaseModule("native") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>& arguments) override {
        // The full flow compares the actual Host image with the central App.
        // The separate wrong-Python-ABI scenario needs only a healthy peer.
        if (arguments.size() == 2) {
            auto expected = vinc::osal::path_from_utf8(arguments[1]);
            auto actual = vinc::osal::executable_path();
            if (!expected || !actual || *expected != *actual) {
                return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                    vinc::module_system::ErrorOperation::StartHook});
            }

            std::cout << "NATIVE_SELF_HOST_OK\n" << std::flush;
        }
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        std::cout << "NATIVE_PID=" << pid << '\n' << std::flush;

        auto publisher = context.advertise<Packet>("sdk/reply");
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        publisher_.emplace(std::move(*publisher));
        auto subscriber = context.subscribe<Packet>("sdk/request", [this](std::unique_ptr<Packet> message) {
            if (message->publisher_name() != "py" || message->data().value != Packet::revision) {
                std::cerr << "INVALID_PYTHON_PAYLOAD\n" << std::flush;
                return;
            }

            message->data().value = Packet::revision + 1;
            auto sent = publisher_->push(*message);
            if (!sent) {
                std::cerr << vinc::error_to_string(sent.error()) << '\n' << std::flush;
            }
        });
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscriber_.emplace(std::move(*subscriber));
        return context.register_rpc<Packet, Packet>("echo", {}, [](const Packet& request, Packet& response) {
            response.data().value = request.data().value.get() + 2;
            return vinc::rpc::RpcStatus::success();
        });
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        std::cout << "NATIVE_ALIVE\n" << std::flush;
        return 0;
    }
};

REGISTER_MODULE(NativePeer, "NativePeer")
