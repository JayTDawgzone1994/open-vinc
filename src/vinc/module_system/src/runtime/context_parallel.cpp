#include <vinc/module/detail/context_runtime.hpp>
#include <vinc/module/detail/topic_scheduler.hpp>
#include <algorithm>
#include <map>

namespace vinc {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
}

struct Context::TopicBatch {
    struct Pending {
        std::shared_ptr<Subscription> subscription;
        message_bus::detail::ReadBatch boundary;
        bool finished = false;
    };
    std::vector<Pending> pending;
    bool captured = false;
    std::size_t cursor = 0;
};

Result<bool> Context::pump_topic(TopicBatch& batch) {
    if (!living()) {
        return false;
    }
    if (!batch.captured) {
        // One finite source ceiling per subscription. No payload is copied here.
        for (auto& item : batch.pending) {
            auto captured = message_bus::detail::capture(item.subscription->core);
            if (!captured) {
                if (captured.error().is(message_bus::ErrorCode::Closed)) {
                    item.finished = true;
                    continue;
                }
                return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Pump, captured.error()));
            }
            item.boundary = std::move(*captured);
        }
        batch.captured = true;
    }

    // One delivered message per turn, rotating subscriptions as well as Topics.
    // Yielding retains the original boundary; a hot publisher cannot extend it.
    for (std::size_t count = 0; count < batch.pending.size() && living(); ++count) {
        auto& item = batch.pending[batch.cursor];
        batch.cursor = (batch.cursor + 1) % batch.pending.size();
        if (item.finished) {
            continue;
        }
        auto& subscription = *item.subscription;
        if (message_bus::detail::closed(subscription.core)) {
            item.finished = true;
            continue;
        }
        auto message = message_bus::detail::read_batch(subscription.core, item.boundary,
            *subscription.schema, subscription.factory);
        if (!message) {
            if (message.error().is(message_bus::ErrorCode::NoNewData) ||
                message.error().is(message_bus::ErrorCode::Closed)) {
                item.finished = true;
                continue;
            }
            return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Pump, message.error()));
        }
        if (!living()) {
            return false;
        }
        if (message_bus::detail::closed(subscription.core)) {
            item.finished = true;
            continue;
        }
        // Context-owned identity crosses the Host/Plugin boundary. Keep it only
        // during dispatch, so an exited worker's OS thread ID cannot reject an
        // unrelated application thread that later reuses that ID.
        struct CallbackScope {
            Context& context;
            std::thread::id thread = std::this_thread::get_id();
            explicit CallbackScope(Context& owner) : context(owner) {
                std::lock_guard lock(context.pump_owner_mutex_);
                context.pump_workers_.push_back(thread);
            }
            ~CallbackScope() {
                std::lock_guard lock(context.pump_owner_mutex_);
                std::erase(context.pump_workers_, thread);
            }
        };
        try {
            CallbackScope scope{*this};
            subscription.callback(std::move(*message));
        } catch (...) {
            const Error error{ErrorCode::HookException, ErrorOperation::SubscriptionCallback};
            runtime_->fail(error);
            return std::unexpected(error);
        }
        return true;
    }
    return false;
}

Result<void> Context::pump_parallel(PumpOptions options) {
    // Declared before the pool so its IDs survive all in-flight worker calls.
    struct WorkerIdentityCleanup {
        Context& context;
        ~WorkerIdentityCleanup() {
            std::lock_guard lock(context.pump_owner_mutex_);
            context.pump_workers_.clear();
        }
    } identity_cleanup{*this};
    {
        std::lock_guard lock(pump_owner_mutex_);
        // Reserve before starting threads; callback admission must not allocate.
        pump_workers_.reserve(options.topic_workers);
    }
    detail::TopicScheduler scheduler{[this] { runtime_->wake(); }};
    auto started = scheduler.start(options.topic_workers);
    if (!started) {
        return std::unexpected(wrap_error(ErrorCode::ThreadFailed, ErrorOperation::Pump, started.error()));
    }
    Result<void> dispatched;
    try {
        dispatched = dispatch_parallel(scheduler);
    } catch (const std::bad_alloc&) {
        dispatched = std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Pump});
        runtime_->fail(dispatched.error());
    } catch (...) {
        dispatched = std::unexpected(Error{ErrorCode::HookException, ErrorOperation::Pump});
        runtime_->fail(dispatched.error());
    }
    // An escaped coordinator exception must start supervision before joining.
    // Otherwise a callback waiting for living()==false could stall unwinding
    // before the lifecycle thread ever gets a chance to set the stop deadline.
    scheduler.stop();
    if (!dispatched) {
        return dispatched;
    }
    if (auto failure = scheduler.failure()) {
        return std::unexpected(*failure);
    }
    return {};
}

// Separate dispatch from pool ownership so every exception crosses the stop
// boundary before the pool destructor joins. Expected Result failures stay typed.
Result<void> Context::dispatch_parallel(detail::TopicScheduler& scheduler) {
    while (living()) {
        if (auto failure = scheduler.failure()) {
            return std::unexpected(*failure);
        }
        auto control = pump_control();
        if (!control) {
            return std::unexpected(control.error());
        }
        if (!living()) {
            break;
        }

        std::map<std::string, std::shared_ptr<TopicBatch>, std::less<>> batches;
        std::vector<std::shared_ptr<Subscription>> retired;
        {
            std::lock_guard lock(subscriptions_mutex_);
            retire_closed(retired);
            for (const auto& subscription : subscriptions_) {
                auto& batch = batches[subscription->topic];
                if (!batch) {
                    batch = std::make_shared<TopicBatch>();
                }
                batch->pending.push_back({subscription, {}, false});
            }
        }
        scheduler.prune();
        for (auto& [topic, batch] : batches) {
            auto task = std::make_shared<detail::TopicScheduler::Task>([this, batch] {
                Result<bool> result;
                try {
                    result = pump_topic(*batch);
                } catch (const std::bad_alloc&) {
                    result = std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Pump});
                    runtime_->fail(result.error());
                } catch (...) {
                    result = std::unexpected(Error{ErrorCode::HookException, ErrorOperation::SubscriptionCallback});
                    runtime_->fail(result.error());
                }
                // Escaped exceptions are terminal; ordinary Result failures
                // retain their existing caller-visible recovery policy.
                return result;
            });
            auto submitted = scheduler.submit(topic, std::move(task));
            if (!submitted) {
                return std::unexpected(submitted.error());
            }
        }

        // Workers continue finite turns independently. Only this caller consumes
        // the Host notification; empty work never wakes it into a resubmit spin.
        if (control->callbacks == 0 && control->commands == 0 && living()) {
            runtime_->wait(Deadline::max());
        }
    }
    return {};
}
}
