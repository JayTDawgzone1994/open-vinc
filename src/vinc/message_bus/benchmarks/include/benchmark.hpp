#pragma once

#include <vinc/module/error.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/osal/path.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace benchmark {
using namespace std::chrono_literals;
inline constexpr std::uint64_t mib = 1024ULL * 1024;
inline constexpr std::uint64_t shared_budget = 4ULL * 1024 * mib;
inline constexpr std::uint64_t image_allowance = 4096;
inline constexpr auto control_poll = 20ms;
inline constexpr auto command_timeout = 10s;
inline constexpr auto drain_timeout = 5s;
inline constexpr auto shutdown_timeout = 15s;
inline constexpr auto warmup_drain = 1s;
inline constexpr auto trial_deadline_margin = 60s;
inline constexpr std::uint64_t trial_deadline_multiplier = 5;
inline constexpr std::uint64_t maximum_payload_bytes = 128 * mib;
inline constexpr std::uint64_t maximum_slots = 65536;
inline constexpr std::uint64_t maximum_period_ns = 1000000000;
inline constexpr std::uint64_t maximum_samples = 100000;
inline constexpr std::uint8_t first_marker = 0x5a;
inline constexpr std::uint8_t last_marker = 0xa5;

inline vinc::Error failure() {
    return {vinc::module_system::ErrorCode::HookFailed, vinc::module_system::ErrorOperation::MainHook};
}

inline std::uint64_t now_ns() {
    return vinc::osal::monotonic_nanoseconds();
}

inline bool number(const std::string& text, std::uint64_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

struct Options {
    std::string role;
    std::string mode;
    std::uint64_t bytes = 0;
    std::uint64_t slots = 0;
    std::uint64_t period_ns = 0;
    std::uint64_t samples = 0;
    std::uint64_t warmup = 0;
    std::filesystem::path directory;
};

inline bool parse(const std::vector<std::string>& args, Options& options) {
    if (args.size() != 8) {
        return false;
    }

    options.role = args[0];
    options.mode = args[1];
    auto directory = vinc::osal::path_from_utf8(args[7]);
    if (!directory) {
        return false;
    }

    options.directory = std::move(*directory);
    const bool parsed = number(args[2], options.bytes) && number(args[3], options.slots) &&
        number(args[4], options.period_ns) && number(args[5], options.samples) && number(args[6], options.warmup);
    if (!parsed) {
        return false;
    }

    return (options.role == "publisher" || options.role == "subscriber") &&
        (options.mode == "push" || options.mode == "loan") &&
        options.bytes >= 2 && options.bytes <= maximum_payload_bytes &&
        options.slots >= 2 && options.slots <= maximum_slots &&
        options.period_ns > 0 && options.period_ns <= maximum_period_ns &&
        options.samples > 0 && options.samples <= maximum_samples &&
        options.warmup >= 2 * options.slots && options.warmup <= maximum_samples;
}

struct Resources {
    bool valid = false;
    std::uint64_t cpu_ns = 0;
    std::uint64_t peak_rss_bytes = 0;
};

// Benchmark instrumentation only: this does not add platform APIs to the SDK.
// Peak RSS is the process-lifetime high-water mark, not exclusive physical memory.
inline Resources resources() {
    Resources result;
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) ||
        !GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        return result;
    }

    // FILETIME consists of two unsigned words encoding 100 ns units.
    const auto kernel_ticks = (std::uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const auto user_ticks = (std::uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    result.cpu_ns = (kernel_ticks + user_ticks) * 100;
    result.peak_rss_bytes = memory.PeakWorkingSetSize;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return result;
    }

    result.cpu_ns = (std::uint64_t(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000000ULL +
        (std::uint64_t(usage.ru_utime.tv_usec) + usage.ru_stime.tv_usec) * 1000;
#ifdef __APPLE__
    result.peak_rss_bytes = usage.ru_maxrss;
#else
    result.peak_rss_bytes = std::uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
    result.valid = true;
    return result;
}

inline bool save_resources(const std::filesystem::path& path, Resources before, Resources after,
                           std::uint64_t start, std::uint64_t end) {
    std::ofstream file(path);
    const bool valid = before.valid && after.valid && after.cpu_ns >= before.cpu_ns && end > start;
    const auto cpu = valid ? after.cpu_ns - before.cpu_ns : 0;
    const auto wall = end >= start ? end - start : 0;
    file << "valid,cpu_ns,wall_ns,peak_rss_bytes\n";
    file << valid << ',' << cpu << ',' << wall << ',' << after.peak_rss_bytes << '\n';
    file.close();
    return !file.fail();
}
}
