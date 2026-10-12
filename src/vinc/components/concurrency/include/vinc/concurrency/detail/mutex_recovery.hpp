#pragma once

#include <vinc/concurrency/interprocess_mutex.hpp>

namespace vinc::concurrency::detail {
// Designated supervisor integration only, never a Module SDK operation.
struct MutexRecovery {
    // PRECONDITION: OSAL confirmed this process exited; its owner cannot be
    // reissued in this epoch. Heartbeat timeout is NOT evidence of exit.
    // Only central_owner may reclaim. No match returns WouldBlock.
    // Publication is preserved. The returned guard excludes other users while
    // the supervisor discards candidate data or repairs an in-place payload.
    [[nodiscard]] static Result<InterprocessMutexGuard> acquire_after_exit(
        const InterprocessMutex& mutex, OwnerId dead_owner);
};
}
