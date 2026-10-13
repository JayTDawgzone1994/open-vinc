#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vinc/result.hpp>

namespace vinc {

// Absolute cutoff for local waits on a monotonic clock, not a wall-clock timestamp.
// Pass one deadline through multiple steps to share a total waiting budget.
// Do not serialize its representation/epoch as a cross-process or network contract.
using Deadline = std::chrono::steady_clock::time_point;

// Unix nanoseconds from system_clock, excluding leap seconds. Reject pre-epoch
// or unrepresentable readings; never reinterpret a negative count as unsigned.
Result<std::uint64_t> unix_nanoseconds() noexcept;

// Fixed UTC text: YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ. Every uint64_t input is valid.
// Does not use the local timezone or a timezone database. The output may allocate.
std::string format_utc(std::uint64_t unix_nanoseconds);

} // namespace vinc
