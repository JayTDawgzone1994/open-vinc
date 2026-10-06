#pragma once

// Internal bookkeeping shared only by the implementation and controlled tests.
#include <vinc/osal/file_lock.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace vinc::concurrency::detail {
struct InterprocessMutexState {
    osal::FileLock file_lock;
    // Protects the reservation and retained FileLock guard. Ownership changes
    // also occur under this mutex; try_lock can inspect the identity separately.
    std::mutex local_mutex;
    std::condition_variable_any local_changed;
    bool occupied = false;
    // Process-local TLS identity, never dereferenced or stored in shared memory.
    // Atomic reads preserve recursive Busy without waiting for local_mutex.
    std::atomic<const void*> owner_thread_token{nullptr};
    std::optional<osal::FileLockGuard> file_guard;
};

// An implementation that hides a lock here would defeat the try_lock contract.
static_assert(std::atomic<const void*>::is_always_lock_free);
}
