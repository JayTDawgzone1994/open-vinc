#include <vinc/module/detail/context_runtime.hpp>
#include <vinc/config.hpp>
#include <algorithm>

namespace vinc {
Result<std::unique_ptr<detail::ContextLock>> detail::ContextLockAccess::lock(
    Context& context, std::uint64_t id, Deadline deadline) {
    if (!context.runtime_ || !context.living()) {
        return std::unexpected(Error{module_system::ErrorCode::ResourcesUnavailable,
                                     module_system::ErrorOperation::AccessLock});
    }
    return context.runtime_->lock(id, deadline);
}

namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;

struct PumpGuard {
    std::atomic_flag& active;
    ~PumpGuard() {
        active.clear();
        active.notify_all();
    }
};
}

bool Context::living() const noexcept {
    return runtime_ && runtime_->living();
}

Result<std::reference_wrapper<message_bus::Client>> Context::client() {
    if (!client_) {
        return std::unexpected(Error{ErrorCode::ResourcesUnavailable, ErrorOperation::AccessMessageBus});
    }

    if (!living()) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::AccessMessageBus});
    }

    return std::ref(*client_);
}

void Context::retire_closed(std::vector<std::shared_ptr<Subscription>>& retired) {
    // Caller holds the registry lock. Release captured user objects only after
    // unlocking; their destructors may themselves call application/SDK code.
    retired.reserve(subscriptions_.size());
    for (auto& item : subscriptions_) {
        if (message_bus::detail::closed(item->core)) {
            retired.push_back(std::move(item));
        }
    }

    std::erase(subscriptions_, nullptr);
}

Result<void> Context::register_callback(std::shared_ptr<Subscription> subscription) {
    std::vector<std::shared_ptr<Subscription>> retired;
    std::lock_guard lock(subscriptions_mutex_);
    retire_closed(retired);
    if (subscriptions_.size() >= config::module_callback_limit) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Pump});
    }

    subscriptions_.push_back(std::move(subscription));
    return {};
}

Result<PumpStats> Context::pump_batch() {
    PumpStats result;
    if (!living()) {
        result.stopped = true;
        return result;
    }

    struct Pending {
        std::shared_ptr<Subscription> subscription;
        message_bus::detail::ReadBatch boundary;
    };
    std::vector<Pending> pending;
    std::vector<std::shared_ptr<Subscription>> retired;
    {
        std::lock_guard lock(subscriptions_mutex_);
        retire_closed(retired);
        pending.reserve(subscriptions_.size());
        for (auto& subscription : subscriptions_) {
            pending.push_back({subscription, {}});
        }
    }

    // Freeze every source boundary before invoking any callback or command hook.
    for (auto& item : pending) {
        auto captured = message_bus::detail::capture(item.subscription->core);
        if (!captured) {
            if (captured.error().is(message_bus::ErrorCode::Closed)) {
                continue;
            }

            return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Pump, captured.error()));
        }

        item.boundary = std::move(*captured);
    }

    auto rpc_work = rpc_.pump();
    if (!rpc_work) {
        auto error = wrap_error(ErrorCode::RpcFailed, ErrorOperation::Pump, rpc_work.error());
        runtime_->fail(error);
        return std::unexpected(error);
    }
    result.callbacks += rpc_work->requests + rpc_work->completions;

    auto parameters = param.pump();
    if (!parameters) {
        if (!living()) {
            result.stopped = true;
            return result;
        }
        if (parameters.error().is(param::ErrorCode::CallbackFailed)) {
            auto error = wrap_error(ErrorCode::HookException, ErrorOperation::ParamCallback, parameters.error());
            runtime_->fail(error);
            return std::unexpected(error);
        }
        return std::unexpected(wrap_error(ErrorCode::ParamFailed, ErrorOperation::Pump, parameters.error()));
    }
    result.callbacks += *parameters;

    auto commands = runtime_->commands();
    if (!commands) {
        return std::unexpected(commands.error());
    }

    result.commands = *commands;
    for (auto& item : pending) {
        auto& subscription = *item.subscription;
        const auto before = message_bus::detail::stats(subscription.core);
        while (living() && !message_bus::detail::closed(subscription.core)) {
            auto message = message_bus::detail::read_batch(subscription.core, item.boundary,
                                                          *subscription.schema, subscription.factory);
            if (!message) {
                if (message.error().is(message_bus::ErrorCode::NoNewData) ||
                    message.error().is(message_bus::ErrorCode::Closed)) {
                    break;
                }

                return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Pump, message.error()));
            }

            if (!living() || message_bus::detail::closed(subscription.core)) {
                break;
            }

            try {
                subscription.callback(std::move(*message));
            } catch (...) {
                const Error error{ErrorCode::HookException, ErrorOperation::SubscriptionCallback};
                // Latch at Host level: ignoring the returned Result cannot keep this Module alive.
                runtime_->fail(error);
                return std::unexpected(error);
            }

            ++result.callbacks;
        }

        result.lost_messages += message_bus::detail::stats(subscription.core).lost_messages - before.lost_messages;
    }

    result.stopped = !living();
    return result;
}

Result<PumpStats> Context::pumpOnce() {
    if (!runtime_) {
        return std::unexpected(Error{ErrorCode::ResourcesUnavailable, ErrorOperation::Pump});
    }

    if (pumping_.test_and_set()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Pump});
    }

    PumpGuard guard{pumping_};
    return pump_batch();
}

Result<void> Context::pumpLoop() {
    if (!runtime_) {
        return std::unexpected(Error{ErrorCode::ResourcesUnavailable, ErrorOperation::Pump});
    }

    if (pumping_.test_and_set()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Pump});
    }

    PumpGuard guard{pumping_};
    while (living()) {
        auto pumped = pump_batch();
        if (!pumped) {
            return std::unexpected(pumped.error());
        }

        if (pumped->callbacks == 0 && pumped->commands == 0 && living()) {
            runtime_->wait();
        }
    }

    return {};
}
}

namespace vinc {
Result<ModulePage> Context::list_modules(MID after, Deadline deadline) {
    if (!runtime_ || !living()) {
        return std::unexpected(Error{module_system::ErrorCode::NotRunning,
            module_system::ErrorOperation::ListModules});
    }

    return runtime_->list_modules(after, deadline);
}
}
