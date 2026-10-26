#pragma once

#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/owner_id.hpp>
#include <atomic>
#include <string>

namespace vinc::concurrency::detail {
inline constexpr std::uint64_t mutex_magic = 0x56494e434d555458ULL;
inline constexpr std::uint32_t mutex_layout_version = 1;
inline constexpr std::uint64_t publication_mask = 1;

struct alignas(8) MutexHeader {
    std::uint64_t magic = mutex_magic;
    std::uint32_t version = mutex_layout_version;
    std::uint32_t bytes = sizeof(MutexHeader);
    osal::SharedWord control{};
};

struct InterprocessMutexState {
    std::string name;
    OwnerId identity = no_owner;
    osal::SharedMemory memory;
    osal::SharedMemoryMapping mapping;
    MutexHeader* header = nullptr;
    // Process-local affinity/recursion only. No blocking bookkeeping mutex.
    std::atomic<const void*> local_owner{nullptr};
};
static_assert(std::atomic<const void*>::is_always_lock_free);
static_assert(sizeof(MutexHeader) == 24);
}
