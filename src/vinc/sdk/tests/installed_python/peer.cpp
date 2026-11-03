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

        auto registered = context.param.register_param("fixed", std::int32_t{7},
            {.restart_hint = vinc::param::RestartHint::Module});
        if (!registered) {
            return std::unexpected(registered.error());
        }

        auto shared = context.param.register_param("shared", std::int32_t{7},
            {.permission = vinc::param::Permission::Public});
        if (!shared) {
            return std::unexpected(shared.error());
        }

        auto plain = context.param.register_param("plain", std::int32_t{1});
        if (!plain) {
            return std::unexpected(plain.error());
        }

        auto default_entry = context.param.read_current("plain");
        if (!default_entry) {
            return std::unexpected(default_entry.error());
        }

        const bool defaults_match = default_entry->permission == vinc::param::Permission::Limited
            && default_entry->restart_hint == vinc::param::RestartHint::None;
        if (!defaults_match) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                vinc::module_system::ErrorOperation::StartHook});
        }

        // Startup reads use the installed Host bridge before the first pump.
        auto current = context.param.read_current("fixed");
        if (!current) {
            return std::unexpected(current.error());
        }

        auto value = current->get<std::int32_t>();
        std::int32_t cached = 0;
        auto cache = context.param.update("fixed", cached);
        const bool cache_is_empty = !cache && cache.error().is(vinc::param::ErrorCode::NotFound);
        const bool metadata_matches = current->permission == vinc::param::Permission::Limited
            && current->restart_hint == vinc::param::RestartHint::Module;
        if (!value || *value != 7 || !metadata_matches || !cache_is_empty) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                vinc::module_system::ErrorOperation::StartHook});
        }

        std::cout << "NATIVE_PARAM_CURRENT_OK\n" << std::flush;
        std::cout << "NATIVE_PARAM_RESTART_OK\n" << std::flush;

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
