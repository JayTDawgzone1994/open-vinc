#pragma once
#include <vinc/osal/error.hpp>
#include <cstddef>
#include <span>

namespace vinc::osal {
// Fill caller-owned bytes with OS cryptographic randomness, or return a native error.
// On failure the destination may be partially written and must not be used as a token.
vinc::Result<void> random_bytes(std::span<std::byte>);
}
