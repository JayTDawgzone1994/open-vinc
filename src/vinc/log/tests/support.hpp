#pragma once
#include <vinc/result.hpp>
#include <vinc/time.hpp>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace log_test {
using namespace vinc;
using namespace std::chrono_literals;
inline constexpr auto timeout = 10s;
inline constexpr auto poll = 20ms;

inline Deadline deadline() {
    return std::chrono::steady_clock::now() + timeout;
}

inline void check(bool value, const char* description) {
    if (!value) {
        throw std::runtime_error(description);
    }
}

template <class T> T take(Result<T> value) {
    if (!value) {
        throw std::runtime_error(error_to_string(value.error()));
    }
    return std::move(*value);
}

inline void take(Result<void> value) {
    if (!value) {
        throw std::runtime_error(error_to_string(value.error()));
    }
}

struct Directory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("vinc-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Directory() {
        std::filesystem::create_directories(path);
    }

    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}
