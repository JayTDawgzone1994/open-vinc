#pragma once

// The small native atomic vocabulary used by shared-memory control structures.
// These operations synchronize metadata; they do not make a whole message copy atomic.

#include <cstdint>
#include <type_traits>

namespace vinc::osal {
// Native, address-independent IPC operations. Storage must be in a live,
// writable shared mapping, aligned to 8 bytes and initialized before sharing.
// Every access after sharing must use these operations. No wait/notify API.
struct alignas(8) SharedWord {
    std::uint64_t value = 0;
};

static_assert(sizeof(SharedWord) == 8 && std::is_trivially_copyable_v<SharedWord>);
// All operations use sequential consistency. On failed compare_exchange,
// expected is replaced with the observed value, ready for the next retry.
std::uint64_t shared_load(const SharedWord&) noexcept;
void shared_store(SharedWord&, std::uint64_t) noexcept;
bool shared_compare_exchange(SharedWord&, std::uint64_t& expected, std::uint64_t desired) noexcept;
std::uint64_t shared_fetch_add(SharedWord&, std::uint64_t) noexcept;
// Same-host monotonic nanoseconds; not UTC, not a cross-machine clock.
std::uint64_t monotonic_nanoseconds() noexcept;
}
