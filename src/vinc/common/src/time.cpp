#include <vinc/time.hpp>
#include <vinc/detail/time.hpp>

namespace vinc {
namespace {
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint64_t seconds_per_minute = 60;
constexpr std::uint64_t seconds_per_hour = 3600;
constexpr std::uint64_t seconds_per_day = 86400;

// The caller supplies calendar fields known to fit the fixed ISO 8601 columns.
void write_digits(std::string& text, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t remaining = width; remaining > 0; --remaining) {
        text[offset + remaining - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}
}

Result<std::uint64_t> unix_nanoseconds() noexcept {
    return detail::unix_nanoseconds_from_duration(std::chrono::system_clock::now().time_since_epoch());
}

std::string format_utc(std::uint64_t timestamp) {
    const auto seconds = timestamp / nanoseconds_per_second;
    const auto day_count = seconds / seconds_per_day;
    const auto day_seconds = seconds % seconds_per_day;
    // Divide before using signed chrono types: UINT64_MAX ns is in year 2554,
    // beyond signed nanoseconds, but its day count fits every supported days rep.
    const auto day = std::chrono::sys_days{std::chrono::days{static_cast<std::chrono::days::rep>(day_count)}};
    const std::chrono::year_month_day date{day};
    std::string text = "0000-00-00T00:00:00.000000000Z";
    write_digits(text, 0, 4, static_cast<unsigned>(static_cast<int>(date.year())));
    write_digits(text, 5, 2, static_cast<unsigned>(date.month()));
    write_digits(text, 8, 2, static_cast<unsigned>(date.day()));
    write_digits(text, 11, 2, day_seconds / seconds_per_hour);
    write_digits(text, 14, 2, (day_seconds % seconds_per_hour) / seconds_per_minute);
    write_digits(text, 17, 2, day_seconds % seconds_per_minute);
    write_digits(text, 20, 9, timestamp % nanoseconds_per_second);
    return text;
}
} // namespace vinc
