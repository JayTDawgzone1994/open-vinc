#pragma once

// Private ownership split between a named memory handle and each local mapping.
// A mapping retains its owner until unmap; neither object stores message or ring metadata.

#include <vinc/osal/error.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <string>

namespace vinc::osal::detail {
struct SharedMemoryState {
    std::intptr_t handle = -1;
    SharedMemoryAccess access = SharedMemoryAccess::ReadOnly;
    std::uint64_t created_size = 0; // unknown for handles obtained by open()
    ~SharedMemoryState();
    vinc::Result<void> close_native() noexcept;
};

struct SharedMemoryMappingState {
    // Keeps the native object alive after the public handle is closed.
    std::shared_ptr<SharedMemoryState> owner;
    std::byte* address = nullptr;
    std::size_t length = 0;
    SharedMemoryAccess access = SharedMemoryAccess::ReadOnly;
    ~SharedMemoryMappingState();
    vinc::Result<void> unmap_native() noexcept;
};

vinc::Result<void> create_memory(SharedMemoryState&, const std::string&, std::uint64_t);
vinc::Result<void> open_memory(SharedMemoryState&, const std::string&, SharedMemoryAccess);
vinc::Result<void> map_memory(SharedMemoryMappingState&);
vinc::Result<void> unlink_memory(const std::string&);
} // namespace vinc::osal::detail
