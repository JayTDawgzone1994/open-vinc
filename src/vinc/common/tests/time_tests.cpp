#include <vinc/time.hpp>
#include <vinc/detail/time.hpp>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        std::cerr << "FAIL: " << reason << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace vinc;
    using namespace std::chrono;
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    check(format_utc(0) == "1970-01-01T00:00:00.000000000Z", "Unix epoch");
    check(format_utc(1) == "1970-01-01T00:00:00.000000001Z", "one nanosecond");
    check(format_utc(86'399'999'999'999) == "1970-01-01T23:59:59.999999999Z", "end of day");
    check(format_utc(86'400'000'000'000) == "1970-01-02T00:00:00.000000000Z", "next day");
    check(format_utc(951'782'400'123'456'789) == "2000-02-29T00:00:00.123456789Z", "leap century");
    check(format_utc(4'107'542'400'000'000'000) == "2100-03-01T00:00:00.000000000Z", "non-leap century");
    check(format_utc(9'223'372'036'854'775'808ULL) == "2262-04-11T23:47:16.854775808Z",
          "beyond signed nanoseconds");
    check(format_utc(maximum) == "2554-07-21T23:34:33.709551615Z", "full uint64 range");

    auto negative = detail::unix_nanoseconds_from_duration(nanoseconds{-1});
    check(!negative && negative.error().is(common::ErrorCode::TimestampOutOfRange) &&
              negative.error().is_operation(common::ErrorOperation::ReadTime), "pre-epoch rejected");
    check(error_to_string(negative.error()).find("System time") != std::string::npos, "time error catalog");
    using WindowsTicks = duration<std::int64_t, std::ratio<1, 10'000'000>>;
    constexpr auto maximum_ticks = static_cast<std::int64_t>(maximum / 100);
    auto accepted = detail::unix_nanoseconds_from_duration(WindowsTicks{maximum_ticks});
    check(accepted && *accepted == maximum - 15, "multiplication upper boundary");
    check(!detail::unix_nanoseconds_from_duration(WindowsTicks{maximum_ticks + 1}), "overflow rejected");
    using UnsignedNanoseconds = duration<std::uint64_t, std::nano>;
    auto full = detail::unix_nanoseconds_from_duration(UnsignedNanoseconds{maximum});
    check(full && *full == maximum, "unsigned nanoseconds retain all bits");
    auto epoch = detail::unix_nanoseconds_from_duration(seconds{0});
    check(epoch && *epoch == 0, "epoch is a valid value");
    const auto before = system_clock::now();
    auto now = unix_nanoseconds();
    const auto after = system_clock::now();
    auto lower = detail::unix_nanoseconds_from_duration(before.time_since_epoch());
    auto upper = detail::unix_nanoseconds_from_duration(after.time_since_epoch());
    check(now && lower && upper && *now >= *lower && *now <= *upper, "system clock sampling");
    std::cout << "PASS: unsigned Unix time and UTC formatting\n";
}
