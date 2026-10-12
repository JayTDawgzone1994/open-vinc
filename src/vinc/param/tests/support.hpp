#pragma once
#include <vinc/result.hpp>
#include <chrono>
#include <filesystem>
#include <random>
#include <stdexcept>

namespace param_test {
inline constexpr auto test_timeout = std::chrono::seconds(10);
inline constexpr auto poll_interval = std::chrono::milliseconds(5);

inline void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    return std::move(*result);
}

inline void take(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

inline std::string unique_name() {
    std::random_device random;
    return "param-" + std::to_string(random()) + "-" + std::to_string(random());
}

inline auto deadline() {
    return std::chrono::steady_clock::now() + test_timeout;
}

struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / unique_name();

    Directory() {
        std::filesystem::create_directory(path);
    }

    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}
