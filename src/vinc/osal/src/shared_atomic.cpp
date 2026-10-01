// Implement sequentially consistent IPC atomics using native 64-bit operations.
// A process-local fallback lock would not protect another process mapping the same word.

#include <vinc/osal/shared_atomic.hpp>
#include <bit>
#include <cstdlib>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <time.h>
#else
#error Unsupported IPC atomic backend
#endif

namespace vinc::osal {
static_assert(sizeof(void*) == 8, "IPC atomics require a 64-bit target");
#ifdef _WIN32
static volatile long long* native(const SharedWord& word) noexcept {
    return reinterpret_cast<volatile long long*>(const_cast<std::uint64_t*>(&word.value));
}
std::uint64_t shared_load(const SharedWord& w) noexcept { return std::bit_cast<std::uint64_t>(_InterlockedCompareExchange64(native(w), 0, 0)); }
void shared_store(SharedWord& w, std::uint64_t v) noexcept { (void)_InterlockedExchange64(native(w), std::bit_cast<long long>(v)); }
std::uint64_t shared_fetch_add(SharedWord& w, std::uint64_t v) noexcept { return std::bit_cast<std::uint64_t>(_InterlockedExchangeAdd64(native(w), std::bit_cast<long long>(v))); }
bool shared_compare_exchange(SharedWord& w, std::uint64_t& expected, std::uint64_t desired) noexcept {
    const auto old = std::bit_cast<std::uint64_t>(_InterlockedCompareExchange64(native(w), std::bit_cast<long long>(desired), std::bit_cast<long long>(expected)));
    if (old == expected) return true;
    expected = old; return false;
}
std::uint64_t monotonic_nanoseconds() noexcept {
    static const auto frequency = [] { LARGE_INTEGER f{}; if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) std::abort(); return static_cast<std::uint64_t>(f.QuadPart); }();
    LARGE_INTEGER counter{}; if (!QueryPerformanceCounter(&counter)) std::abort();
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split to avoid multiplying the long-running counter by 1e9.
    return (ticks / frequency) * 1000000000ULL + static_cast<std::uint64_t>((static_cast<long double>(ticks % frequency) * 1000000000.0L) / frequency);
}
#else
static_assert(__atomic_always_lock_free(8, nullptr), "IPC 64-bit atomics must not fall back to process-local locks");
std::uint64_t shared_load(const SharedWord& w) noexcept { return __atomic_load_n(&w.value, __ATOMIC_SEQ_CST); }
void shared_store(SharedWord& w, std::uint64_t v) noexcept { __atomic_store_n(&w.value, v, __ATOMIC_SEQ_CST); }
std::uint64_t shared_fetch_add(SharedWord& w, std::uint64_t v) noexcept { return __atomic_fetch_add(&w.value, v, __ATOMIC_SEQ_CST); }
bool shared_compare_exchange(SharedWord& w, std::uint64_t& e, std::uint64_t d) noexcept { return __atomic_compare_exchange_n(&w.value, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
std::uint64_t monotonic_nanoseconds() noexcept {
    timespec t{}; if (clock_gettime(CLOCK_MONOTONIC, &t)) std::abort();
    return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(t.tv_nsec);
}
#endif
}
