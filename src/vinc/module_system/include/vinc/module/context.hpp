#pragma once
#include <vinc/message_bus/detail/dispatch.hpp>
#include <vinc/module/error.hpp>
#include <atomic>
#include <functional>
#include <mutex>

namespace vinc {
namespace detail {
class ModuleObject;
class ModuleMessageBusAccess;
class ContextRuntime;
}

struct PumpStats {
    std::uint64_t callbacks = 0;
    std::uint64_t commands = 0;
    std::uint64_t lost_messages = 0;
    bool stopped = false;
};

// Bound once by ModuleHost after central admission. Its address is process-local;
// authority comes from the Host's authenticated session, never from a copied MID.
class Context {
    friend class detail::ModuleObject;
    friend class detail::ModuleMessageBusAccess;
    struct Subscription {
        std::shared_ptr<message_bus::detail::SubscriberCore> core;
        const message::detail::Schema* schema;
        message_bus::detail::Factory factory;
        std::function<void(std::unique_ptr<BaseMessage>)> callback;
    };
    Context() = default;
    std::shared_ptr<message_bus::Client> client_;
    detail::ContextRuntime* runtime_ = nullptr;
    std::mutex subscriptions_mutex_;
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;

    Result<std::reference_wrapper<message_bus::Client>> client();
    void retire_closed(std::vector<std::shared_ptr<Subscription>>& retired);
    Result<void> register_callback(std::shared_ptr<Subscription>);
    Result<PumpStats> pump_batch();

public:
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    bool living() const noexcept;
    Result<PumpStats> pumpOnce();
    Result<void> pumpLoop();

    template <message_bus::Message T>
    Result<message_bus::Publisher<T>> advertise(std::string topic, message_bus::PublisherOptions options = {}) {
        auto access = client();
        if (!access) {
            return std::unexpected(access.error());
        }

        auto result = access->get().advertise<T>(std::move(topic), options);
        if (!result) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, result.error()));
        }

        return result;
    }

    template <message_bus::Message T>
    Result<message_bus::Subscriber<T>> subscribe(std::string topic, message_bus::SubscriberOptions options = {}) {
        auto access = client();
        if (!access) {
            return std::unexpected(access.error());
        }

        auto result = access->get().subscribe<T>(std::move(topic), options);
        if (!result) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, result.error()));
        }

        return result;
    }

    template <message_bus::Message T, class Callback>
        requires std::is_invocable_r_v<void, Callback&, std::unique_ptr<T>>
    Result<message_bus::Subscriber<T>> subscribe(std::string topic, Callback callback,
                                                message_bus::SubscriberOptions options = {}) {
        static_assert(std::is_same_v<std::invoke_result_t<Callback&, std::unique_ptr<T>>, void>,
                      "Subscription callbacks return void");
        auto subscriber = subscribe<T>(std::move(topic), options);
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        auto subscription = std::make_shared<Subscription>();
        subscription->core = message_bus::detail::DispatchAccess::core(*subscriber);
        subscription->schema = &T::schema();
        subscription->factory = [](message::detail::Binding binding) -> std::unique_ptr<BaseMessage> {
            return std::unique_ptr<T>(new T(message::detail::RecordAccess::bind<T>(std::move(binding))));
        };
        subscription->callback = [callback = std::move(callback)](std::unique_ptr<BaseMessage> value) mutable {
            callback(std::unique_ptr<T>(static_cast<T*>(value.release())));
        };
        auto registered = register_callback(std::move(subscription));
        if (!registered) {
            return std::unexpected(registered.error());
        }

        return subscriber;
    }
};
}
