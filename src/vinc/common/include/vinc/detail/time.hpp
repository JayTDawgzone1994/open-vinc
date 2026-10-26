#pragma once

#include <vinc/common_error.hpp>
#include <vinc/result.hpp>
#include <chrono>
#include <limits>

namespace vinc::detail {
// Supported system clocks use integral ticks that divide a nanosecond-based
// second exactly (Windows: 100 ns; Linux/macOS: ns or us). Fail compilation for
// another representation instead of silently truncating or rounding its epoch.
template <class Rep, class Period>
Result<std::uint64_t> unix_nanoseconds_from_duration(std::chrono::duration<Rep, Period> elapsed) noexcept {
    constexpr std::uint64_t nanoseconds_in_second = 1'000'000'000;
    static_assert(std::is_integral_v<Rep> && sizeof(Rep) <= sizeof(std::uint64_t));
    static_assert(Period::num == 1 && nanoseconds_in_second % Period::den == 0);
    constexpr auto nanoseconds_per_tick = nanoseconds_in_second / Period::den;
    if constexpr (std::is_signed_v<Rep>) {
        if (elapsed.count() < 0) {
            return std::unexpected(Error{common::ErrorCode::TimestampOutOfRange, common::ErrorOperation::ReadTime});
        }
    }

    const auto ticks = static_cast<std::uint64_t>(elapsed.count());
    if (ticks > (std::numeric_limits<std::uint64_t>::max)() / nanoseconds_per_tick) {
        return std::unexpected(Error{common::ErrorCode::TimestampOutOfRange, common::ErrorOperation::ReadTime});
    }

    return ticks * nanoseconds_per_tick;
}
} // namespace vinc::detail
