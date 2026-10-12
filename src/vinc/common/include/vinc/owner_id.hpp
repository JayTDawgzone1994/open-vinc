#pragma once

#include <cstdint>

namespace vinc {
// Shared synchronization reserves one bit for publication. Module identity and
// synchronization share this boundary without depending on each other.
using OwnerId = std::uint64_t;
inline constexpr OwnerId no_owner = 0;
inline constexpr OwnerId central_owner = (OwnerId{1} << 63) - 1;
inline constexpr OwnerId max_module_owner = central_owner - 1;

constexpr bool valid_owner(OwnerId owner) noexcept {
    return owner != no_owner && owner <= central_owner;
}
}
