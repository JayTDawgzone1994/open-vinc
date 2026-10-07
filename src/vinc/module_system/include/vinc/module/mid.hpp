#pragma once
#include <cstdint>
#include <string>
#include <variant>

namespace vinc {
// Nonzero, never reused during this central process lifetime, including across
// ModuleManager objects. A restarted child receives a new MID; OS PIDs are separate.
using MID = std::uint64_t;
using ModuleKey = std::variant<MID, std::string>;
}
