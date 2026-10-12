#pragma once

#include <vinc/module/mid.hpp>
#include <vinc/module/error.hpp>
#include <atomic>

namespace vinc::detail {
// The manager owns the process-wide counter. A reference lets boundary tests
// exercise exhaustion without modifying the live manager's identity state.
inline Result<MID> allocate_mid(std::atomic<MID>& next) noexcept {
    auto candidate = next.load(std::memory_order_relaxed);
    for (;;) {
        if (!valid_mid(candidate)) {
            return std::unexpected(make_error(module_system::ErrorCode::MidExhausted,
                                               module_system::ErrorOperation::AllocateMid));
        }

        if (next.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed)) {
            return candidate;
        }
    }
}
}
