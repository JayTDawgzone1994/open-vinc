#pragma once

#include <vinc/osal/shared_memory.hpp>
#include <optional>

namespace vinc::osal::detail {
// Callers that reserve a name before creation need both the primary failure
// and evidence about ownership. Each call clears the previous cleanup error.
struct SharedMemoryCreateAttempt {
    [[nodiscard]] Result<SharedMemory> create(std::string_view name, std::uint64_t byte_size);

    // Present only if this attempt acquired the name and could not confirm
    // its removal on failure. Keep the name for recovery in that case.
    std::optional<Error> name_cleanup_error;
};
} // namespace vinc::osal::detail
