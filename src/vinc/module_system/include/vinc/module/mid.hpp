#pragma once
#include <cstdint>
#include <vinc/owner_id.hpp>
#include <string>
#include <variant>

namespace vinc {
// Nonzero, never reused during this central process lifetime, including across
// ModuleManager objects. A restarted child receives a new MID; OS PIDs are separate.
using MID = std::uint64_t;
// Zero means no identity. The top 63-bit value belongs to the central process,
// never to a Module. Exhaustion is an error; neither IDs nor counters wrap.
inline constexpr MID invalid_mid = no_owner;
inline constexpr MID max_mid = max_module_owner;
constexpr bool valid_mid(MID mid) noexcept {
    return mid > invalid_mid && mid <= max_mid;
}
using ModuleKey = std::variant<MID, std::string>;
}
