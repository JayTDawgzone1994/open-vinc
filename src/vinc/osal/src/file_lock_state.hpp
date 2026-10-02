#pragma once
#include <vinc/osal/file_lock.hpp>
#include <cstdint>
#include <mutex>

namespace vinc::osal::detail {
struct FileLockState {
    std::intptr_t handle = -1;
    // Serializes resource bookkeeping only; no thread owner or recursion rules.
    std::mutex gate;
    bool occupied = false; // one pending acquisition OR one live guard
    ~FileLockState();
};
Result<void> open_file_lock(FileLockState&, const std::filesystem::path&, bool create);
Result<void> try_lock_file_lock(FileLockState&);
Result<void> unlock_file_lock(FileLockState&);
} // namespace vinc::osal::detail
