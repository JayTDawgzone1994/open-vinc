#pragma once
#include <vinc/log/log.hpp>
#include <vinc/time.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace vinc::log {
struct FileOptions {
    std::uint64_t max_file_bytes = config::log_file_bytes;
    std::uint64_t max_total_bytes = config::log_total_bytes;
};
enum class Origin {
    Structured,
    Stdout,
    Stderr,
    Diagnostic
};

struct Record {
    std::string instance;
    std::string startup;
    std::string module;
    std::uint64_t mid = 0;
    std::uint64_t pid = 0;
    std::uint64_t sequence = 0;
    Origin origin = Origin::Structured;
    std::optional<Level> level;
    std::optional<std::int64_t> produced_us;
    std::chrono::system_clock::time_point received;
    Deadline monotonic;
    std::string text;
};

struct View {
    bool enabled = false;
    Level level = Level::Info;
    std::string module;
    std::uint64_t cursor = 0;
};

// Escapes control bytes and preserves printable UTF-8. Each record occupies one line.
std::string render(const Record& record);
std::string date_folder(std::chrono::system_clock::time_point time);
}
