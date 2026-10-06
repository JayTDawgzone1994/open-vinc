#pragma once

#include <chrono>

namespace vinc {

// Absolute cutoff for local waits on a monotonic clock, not a wall-clock timestamp.
// Pass one deadline through multiple steps to share a total waiting budget.
// Do not serialize its representation/epoch as a cross-process or network contract.
using Deadline = std::chrono::steady_clock::time_point;

} // namespace vinc
