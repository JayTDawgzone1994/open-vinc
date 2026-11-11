#pragma once
#include <vinc/message_bus/detail/dispatch.hpp>
#include <vinc/module/error.hpp>
#include <vinc/module/pump_options.hpp>
#include <vinc/module/module_info.hpp>
#include <vinc/param/params.hpp>
#include <vinc/log/log.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc/rpc/detail/rpc_client_access.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace vinc {
namespace detail {
class ModuleObject;
class ModuleMessageBusAccess;
class ContextRuntime;
class TopicScheduler;
struct ContextLockAccess;
struct ContextLanguageAccess;
struct ContextRpcAccess;
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
    friend struct detail::ContextLanguageAccess;
    friend struct detail::ContextRpcAccess;
    friend struct detail::ContextLockAccess;
    friend class detail::ModuleObject;
    friend class detail::ModuleMessageBusAccess;
    struct Subscription {
        std::string topic;
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
    // Object-owned state is shared by Host and Plugin calls; DLL-local TLS is
    // insufficient when command dispatch crosses their runtime boundary.
    std::mutex pump_owner_mutex_;
    std::condition_variable pump_finished_;
    std::thread::id pump_owner_;
    std::vector<std::thread::id> pump_workers_;
    rpc::Client rpc_;

    Result<std::reference_wrapper<message_bus::Client>> client();
    void retire_closed(std::vector<std::shared_ptr<Subscription>>& retired);
    Result<void> register_callback(std::shared_ptr<Subscription>);
    Result<PumpStats> pump_batch();
    Result<PumpStats> pump_control();
    Result<void> pump_parallel(PumpOptions options);
    Result<void> dispatch_parallel(detail::TopicScheduler& scheduler);
    struct TopicBatch;
    Result<bool> pump_topic(TopicBatch& batch);
    Result<rpc::detail::Address> prepare_rpc_call(std::string_view address);

public:
    template<rpc::Message Request, rpc::Message Response>
    Result<rpc::RpcStatus> call_rpc(std::string_view address, rpc::RpcMessages<Request, Response>& messages,
                                   rpc::CallOptions options = {}) {
        auto target = prepare_rpc_call(address);
        if (!target) {
            return std::unexpected(target.error());
        }

        return rpc_.call_sync(*target, messages, options);
    }

    rpc::Stats rpc_stats() const {
        return rpc_.stats();
    }
    template<class Request, class Response, class Handler>
    Result<void> register_rpc(std::string name, Handler handler, rpc::ServiceOptions options = {}) {
        return rpc_.register_rpc<Request, Response>(std::move(name), options, std::move(handler));
    }
    template<rpc::Message Request, rpc::Message Response, class Completion>
    Result<rpc::RpcClient<Request, Response>> rpc_client(std::string_view address,
        Completion completion, rpc::CallOptions options = {}) {
        if (!living()) {
            return std::unexpected(Error{rpc::ErrorCode::Closed, rpc::ErrorOperation::Create});
        }

        return rpc::detail::RpcClientAccess::create<Request, Response>(
            rpc::detail::ClientAccess::endpoint(rpc_), address, std::move(completion), options);
    }
    param::Params param;
    log::Logger log;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    bool living() const noexcept;
    // Read-only pages have exclusive cursors and one caller-supplied deadline.
    // Concurrent registry changes can appear between pages. No global snapshot is promised.
    Result<ModulePage> list_modules(MID after, Deadline deadline);
    Result<message_bus::TopicConnectionsPage> list_topic_connections(
        message_bus::ConnectionCursor after, Deadline deadline) {
        auto access = client();
        if (!access) {
            return std::unexpected(access.error());
        }

        auto result = access->get().list_topic_connections(std::move(after), deadline);
        if (!result) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, result.error()));
        }

        return result;
    }
    Result<PumpStats> pumpOnce();
    Result<void> pumpLoop(PumpOptions options = {});
    // Wait for a hint, stop, or the earlier of deadline and the fallback scan.
    // This never dispatches callbacks; success does not prove work is available.
    // Shares the single active caller with pumpOnce/pumpLoop. Pump again after waking.
    Result<void> waitForWork(Deadline deadline = Deadline::max());

    Result<message_bus::TopicPage> list_topics(std::string after = {}) {
        auto access = client();
        if (!access) {
            return std::unexpected(access.error());
        }
        auto result = access->get().list_topics(std::move(after));
        if (!result) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, result.error()));
        }
        return result;
    }

    Result<message_bus::TopicDiagnosticsPage> inspect_topics(std::string after = {}) {
        auto access = client();
        if (!access) {
            return std::unexpected(access.error());
        }

        auto result = access->get().inspect_topics(std::move(after));
        if (!result) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, result.error()));
        }

        return result;
    }

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
        auto subscriber = subscribe<T>(topic, options);
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        auto notifications = message_bus::detail::enable_notifications(
            message_bus::detail::DispatchAccess::core(*subscriber));
        if (!notifications) {
            return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
                module_system::ErrorOperation::AccessMessageBus, notifications.error()));
        }

        auto subscription = std::make_shared<Subscription>();
        subscription->topic = std::move(topic);
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
