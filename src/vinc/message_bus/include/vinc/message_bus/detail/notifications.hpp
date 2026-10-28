#pragma once

#include <vinc/config.hpp>
#include <vinc/message_bus/error.hpp>
#include <vinc/osal/notification.hpp>
#include <vinc/time.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#ifdef VINC_MESSAGE_BUS_TESTING
#include "bus_test_hooks.hpp"
#endif

namespace vinc::message_bus::detail {
struct NotificationTarget {
    std::uint64_t owner = 0;
    std::string name;
};

struct NotificationDiagnostics {
    std::uint64_t target = 0;
    std::uint64_t failures = 0;
    std::uint64_t retries = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t first_failure = 0;
    bool degraded = false;
    std::optional<Error> last_error;
};

// One bounded process-local summary survives individual route retirement.
struct NotificationTotals {
    std::mutex mutex;
    NotificationDiagnostics value;
    std::uint64_t degraded_routes = 0;
};

// One destination per Publisher/Module generation. The short per-destination lock
// protects its handle and diagnostics, never Bus topology or application callbacks.
// Opening a replacement happens outside that lock on the existing receive worker.
class NotificationDestination {
    NotificationTarget target_;
    std::shared_ptr<NotificationTotals> totals_;
    mutable std::mutex mutex_;
    osal::NotificationSender sender_;
    NotificationDiagnostics diagnostics_;
    Deadline retry_at_{};
    std::uint32_t consecutive_failures_ = 0;

    void failed(const Error& error, Deadline now) {
        if (!diagnostics_.first_failure) {
            auto timestamp = unix_nanoseconds();
            diagnostics_.first_failure = timestamp ? *timestamp : 0;
        }
        std::lock_guard total_lock(totals_->mutex);
        auto& total = totals_->value;
        total.target = target_.owner;
        ++total.failures;
        if (!diagnostics_.degraded) {
            ++totals_->degraded_routes;
        }
        total.degraded = true;
        if (!total.first_failure) {
            total.first_failure = diagnostics_.first_failure;
        }
        total.last_error = error;
        ++diagnostics_.failures;
        diagnostics_.degraded = true;
        diagnostics_.last_error = error;
        ++consecutive_failures_;
        retry_at_ = now + config::notification_retry_interval;
    }

    void succeeded() {
        if (diagnostics_.degraded) {
            ++diagnostics_.recoveries;
            std::lock_guard total_lock(totals_->mutex);
            ++totals_->value.recoveries;
            --totals_->degraded_routes;
            totals_->value.degraded = totals_->degraded_routes != 0;
        }
        diagnostics_.degraded = false;
        consecutive_failures_ = 0;
    }

public:
    explicit NotificationDestination(NotificationTarget target, std::shared_ptr<NotificationTotals> totals)
        : target_(std::move(target)), totals_(std::move(totals)) {
        diagnostics_.target = target_.owner;
    }

    ~NotificationDestination() {
        // Last snapshot release proves no notifier can still use this route.
        if (diagnostics_.degraded) {
            std::lock_guard total_lock(totals_->mutex);
            --totals_->degraded_routes;
            totals_->value.degraded = totals_->degraded_routes != 0;
        }
    }

    const NotificationTarget& target() const noexcept {
        return target_;
    }

    void reopen(Deadline now) {
        {
            std::lock_guard lock(mutex_);
            if (sender_ && !diagnostics_.degraded) {
                return;
            }
            if (now < retry_at_) {
                return;
            }
            ++diagnostics_.retries;
            {
                std::lock_guard total_lock(totals_->mutex);
                ++totals_->value.retries;
            }
            retry_at_ = now + config::notification_retry_interval;
        }

        auto opened = osal::NotificationSender::open(target_.name);
        std::lock_guard lock(mutex_);
        if (!opened) {
            failed(wrap_error(ErrorCode::Io, ErrorOperation::OpenNotification, opened.error()), now);
            return;
        }

        sender_ = std::move(*opened);
        // Catch up even when no further publication follows route installation.
        auto sent = [&] {
#ifdef VINC_MESSAGE_BUS_TESTING
            return testing::notification_notify(sender_, target_.owner);
#else
            return sender_.notify();
#endif
        }();
        if (!sent) {
            failed(wrap_error(ErrorCode::Io, ErrorOperation::Notify, sent.error()), now);
            return;
        }
        succeeded();
    }

    void notify(Deadline now) {
        std::lock_guard lock(mutex_);
        const bool backing_off = consecutive_failures_ >= config::notification_send_failures_before_backoff &&
                                 now < retry_at_;
        if (!sender_ || backing_off) {
            return;
        }

        auto sent = [&] {
#ifdef VINC_MESSAGE_BUS_TESTING
            return testing::notification_notify(sender_, target_.owner);
#else
            return sender_.notify();
#endif
        }();
        if (!sent) {
            failed(wrap_error(ErrorCode::Io, ErrorOperation::Notify, sent.error()), now);
            return;
        }
        succeeded();
    }

    NotificationDiagnostics diagnostics() const {
        std::lock_guard lock(mutex_);
        return diagnostics_;
    }
};

using NotificationDestinations = std::vector<std::shared_ptr<NotificationDestination>>;

// Publication holds a snapshot, so retiring a route cannot close an in-flight
// sender. Only topology updates allocate; the normal signalling path does not.
struct NotificationRoutes {
    std::atomic<std::shared_ptr<const NotificationDestinations>> destinations;

    void notify() const {
        auto current = destinations.load();
        if (!current || current->empty()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& destination : *current) {
            destination->notify(now);
        }
    }
};
}
