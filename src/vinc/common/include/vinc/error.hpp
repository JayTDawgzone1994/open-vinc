#pragma once

#include <cstdint>
#include <string>

namespace vinc {

// The framework error catalog will be added as components adopt Result<T>.
// No numeric value, including zero, has an assigned meaning yet.
enum class ErrorCode : std::int32_t {
};

// Common failure payload. Success is represented by Result<T>'s value state.
struct Error {
    ErrorCode code{};
};

// Return an owned diagnostic string. Unknown codes retain their numeric value.
// Formatting may allocate, so this function is intentionally not noexcept.
std::string error_to_string(const Error& error);

} // namespace vinc
