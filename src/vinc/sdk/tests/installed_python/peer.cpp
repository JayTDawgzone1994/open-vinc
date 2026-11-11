#include <vinc/vinc.hpp>
#include <vinc_messages/SdkPacket.hpp>
#include <iostream>
#include <optional>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

class NativePeer final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    using Packet = vinc::SdkPacket;
    std::optional<vinc::message_bus::Publisher<Packet>> publisher_;
    std::optional<vinc::message_bus::Subscriber<Packet>> subscriber_;
    std::optional<vinc::message_bus::Publisher<Packet>> second_publisher_;
    std::optional<vinc::message_bus::Subscriber<Packet>> second_subscriber_;
    std::mutex gate_mutex_;
    std::condition_variable gate_changed_;
    std::array<std::thread::id, 2> callback_threads_{};
    std::array<std::unique_ptr<Packet>, 2> retained_;
    std::size_t arrived_ = 0;

    void receive(std::size_t index, std::unique_ptr<Packet> message) {
        constexpr auto overlap_timeout = std::chrono::seconds(5);
        if (message->publisher_name() != "py" || message->data().value != Packet::revision) {
            std::cerr << "INVALID_PYTHON_PAYLOAD\n" << std::flush;
            return;
        }

        // The first callback cannot finish before the other Topic enters. The
        // condition-variable wait releases the application mutex, not pump ownership.
        {
            std::unique_lock lock(gate_mutex_);
            callback_threads_[index] = std::this_thread::get_id();
            ++arrived_;
            gate_changed_.notify_all();
            if (!gate_changed_.wait_for(lock, overlap_timeout, [this] { return arrived_ == 2; })
                || callback_threads_[0] == callback_threads_[1]) {
                std::cerr << "INVALID_PYTHON_PAYLOAD: native parallel gate\n" << std::flush;
                return;
            }
        }

        message->data().value = Packet::revision + 1;
        auto& publisher = index == 0 ? publisher_ : second_publisher_;
        auto sent = publisher->push(*message);
        if (!sent) {
            std::cerr << vinc::error_to_string(sent.error()) << '\n' << std::flush;
            return;
        }

        // Each index has one writer. Runtime drains both before on_stop reads.
        retained_[index] = std::move(message);
    }

public:
    NativePeer() : BaseModule("native", {.topic_workers = 2}) {
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
            receive(0, std::move(message));
        });
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        subscriber_.emplace(std::move(*subscriber));
        auto second_publisher = context.advertise<Packet>("sdk/reply2");
        if (!second_publisher) {
            return std::unexpected(second_publisher.error());
        }

        second_publisher_.emplace(std::move(*second_publisher));
        auto second_subscriber = context.subscribe<Packet>("sdk/request2", [this](std::unique_ptr<Packet> message) {
            receive(1, std::move(message));
        });
        if (!second_subscriber) {
            return std::unexpected(second_subscriber.error());
        }

        second_subscriber_.emplace(std::move(*second_subscriber));
        return context.register_rpc<Packet, Packet>("echo", [](const Packet& request, Packet& response) {
            response.data().value = request.data().value.get() + 2;
            return vinc::rpc::RpcStatus::success();
        }, {});
    }

    vinc::Result<void> on_stop() override {
        // Wrong-Python-ABI tests deliberately leave this peer without deliveries.
        if (retained_[0] && retained_[1]
            && retained_[0]->data().value == Packet::revision + 1
            && retained_[1]->data().value == Packet::revision + 1) {
            std::cout << "INSTALLED_CPP_PARALLEL_OK\n" << std::flush;
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        std::cout << "NATIVE_ALIVE\n" << std::flush;
        return 0;
    }
};

REGISTER_MODULE(NativePeer, "NativePeer")
