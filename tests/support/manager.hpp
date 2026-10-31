#pragma once
#include <vinc/module/detail/manager_access.hpp>
#include <vinc_test/host_path.hpp>
#include <vinc/osal/path.hpp>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>

namespace vinc::test {
// CTest selects the real App only for entry integration cases. Production
// launch selection never reads this test-only environment variable.
inline std::filesystem::path host_path() {
#ifdef _WIN32
    wchar_t* value = nullptr;
    std::size_t units = 0;
    const auto copied = _wdupenv_s(&value, &units, L"VINC_TEST_HOST_PATH");
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    if (copied != 0) {
        throw std::runtime_error("Cannot read VINC_TEST_HOST_PATH");
    }

    const auto* selected = owned.get();
#else
    const auto* selected = std::getenv("VINC_TEST_HOST_PATH");
#endif
    if (!selected) {
        return std::filesystem::path(VINC_TEST_HOST_PATH);
    }

#ifdef _WIN32
    Result<std::filesystem::path> path = std::filesystem::path(selected);
#else
    auto path = osal::path_from_utf8(selected);
#endif
    if (!path || !path->is_absolute()) {
        throw std::runtime_error("VINC_TEST_HOST_PATH must be an absolute UTF-8 path");
    }

    return std::move(*path);
}

inline std::string instance_name() {
    std::random_device random;
    return "test-" + std::to_string(random()) + "-" + std::to_string(random());
}

inline Result<std::unique_ptr<ModuleManager>> create_manager(message_bus::Limits limits = {},
    std::filesystem::path host = host_path()) {
    return detail::ManagerAccess::create(instance_name(), limits, std::move(host));
}
}
