#pragma once

#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/concurrency/detail/interprocess_mutex_state.hpp>

namespace vinc::concurrency::detail {
// Tests may pause bookkeeping, but must not mutate ownership or the native lock.
struct InterprocessMutexTestAccess {
    static std::mutex& bookkeeping_mutex(const InterprocessMutex& mutex) {
        return mutex.state_->local_mutex;
    }
};
}
