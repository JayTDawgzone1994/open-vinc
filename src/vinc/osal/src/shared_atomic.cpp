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
namespace {
constexpr auto nanoseconds_per_second = 1000000000ULL;
#ifdef _WIN32
constexpr auto nanoseconds_per_second_fractional = 1000000000.0L;
#endif
}

static_assert(sizeof(void*) == 8, "IPC atomics require a 64-bit target");
#ifdef _WIN32
static volatile long long* native(const SharedWord& word) noexcept {
    // MSVC's intrinsic takes signed 64-bit storage; uint64_t is its unsigned
    // counterpart on this backend. SharedWord guarantees eight-byte alignment.
    // Even load uses a read-modify-write intrinsic: callers must supply a live,
    // writable mapping, never an actually const object or a read-only view.
    return reinterpret_cast<volatile long long*>(const_cast<std::uint64_t*>(&word.value));
}

std::uint64_t shared_load(const SharedWord& word) noexcept {
    return std::bit_cast<std::uint64_t>(_InterlockedCompareExchange64(native(word), 0, 0));
}

void shared_store(SharedWord& word, std::uint64_t value) noexcept {
    (void)_InterlockedExchange64(native(word), std::bit_cast<long long>(value));
}

std::uint64_t shared_fetch_add(SharedWord& word, std::uint64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(
        _InterlockedExchangeAdd64(native(word), std::bit_cast<long long>(value)));
}

bool shared_compare_exchange(SharedWord& word, std::uint64_t& expected, std::uint64_t desired) noexcept {
    const auto observed = std::bit_cast<std::uint64_t>(_InterlockedCompareExchange64(
        native(word), std::bit_cast<long long>(desired), std::bit_cast<long long>(expected)));
    if (observed == expected) {
        return true;
    }
    expected = observed;
    return false;
}

std::uint64_t monotonic_nanoseconds() noexcept {
    static const auto frequency = [] {
        LARGE_INTEGER native_frequency{};
        if (!QueryPerformanceFrequency(&native_frequency) || native_frequency.QuadPart <= 0) {
            std::abort();
        }

        return static_cast<std::uint64_t>(native_frequency.QuadPart);
    }();
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        std::abort();
    }

    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split to avoid multiplying the long-running counter by 1e9.
    const auto whole_seconds_ns = (ticks / frequency) * nanoseconds_per_second;
    const auto fractional_seconds_ns = static_cast<std::uint64_t>(
        (static_cast<long double>(ticks % frequency) * nanoseconds_per_second_fractional) / frequency);
    return whole_seconds_ns + fractional_seconds_ns;
}
#else
static_assert(__atomic_always_lock_free(8, nullptr),
              "IPC 64-bit atomics must not fall back to process-local locks");

std::uint64_t shared_load(const SharedWord& word) noexcept {
    return __atomic_load_n(&word.value, __ATOMIC_SEQ_CST);
}

void shared_store(SharedWord& word, std::uint64_t value) noexcept {
    __atomic_store_n(&word.value, value, __ATOMIC_SEQ_CST);
}

std::uint64_t shared_fetch_add(SharedWord& word, std::uint64_t value) noexcept {
    return __atomic_fetch_add(&word.value, value, __ATOMIC_SEQ_CST);
}

bool shared_compare_exchange(SharedWord& word, std::uint64_t& expected, std::uint64_t desired) noexcept {
    return __atomic_compare_exchange_n(&word.value, &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

std::uint64_t monotonic_nanoseconds() noexcept {
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp)) {
        std::abort();
    }

    return static_cast<std::uint64_t>(timestamp.tv_sec) * nanoseconds_per_second +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}
#endif
}
