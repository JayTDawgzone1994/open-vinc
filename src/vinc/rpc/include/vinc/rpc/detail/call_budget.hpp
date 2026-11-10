#pragma once
#include <vinc/rpc/types.hpp>
#include <mutex>

namespace vinc::rpc::detail {
class CallBudget;

// Host retirement may outlive the waiting caller, but never retains a Plugin
// callback. This lease keeps unreclaimed sync and async calls in one budget.
struct CallReservation {
    std::shared_ptr<CallBudget> budget;
    std::uint64_t bytes = 0;
    CallReservation() = default;
    CallReservation(const CallReservation&) = delete;
    CallReservation& operator=(const CallReservation&) = delete;
    ~CallReservation();
};

class CallBudget {
    friend struct CallReservation;
    std::mutex mutex_;
    std::uint64_t pending_ = 0;
    std::uint64_t bytes_ = 0;

public:
    static Result<std::shared_ptr<CallReservation>> reserve(
        const std::shared_ptr<CallBudget>& budget, std::uint64_t bytes) {
        std::lock_guard lock(budget->mutex_);
        if (budget->pending_ >= config::rpc_pending_limit || bytes > config::rpc_local_bytes - budget->bytes_) {
            return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Call});
        }

        // Construct before charging: allocation failure must not consume capacity.
        auto reservation = std::make_shared<CallReservation>();
        reservation->budget = budget;
        reservation->bytes = bytes;
        ++budget->pending_;
        budget->bytes_ += bytes;
        return reservation;
    }

    void inspect(Stats& stats) {
        std::lock_guard lock(mutex_);
        stats.pending = pending_;
        stats.request_bytes = bytes_;
    }
};

inline CallReservation::~CallReservation() {
    if (budget) {
        std::lock_guard lock(budget->mutex_);
        --budget->pending_;
        budget->bytes_ -= bytes;
    }
}
}
