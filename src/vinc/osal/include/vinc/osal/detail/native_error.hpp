#pragma once

#include <vinc/osal/error.hpp>
#include <system_error>

namespace vinc::osal::detail {
#if defined(_WIN32)
inline constexpr NativeErrorSource errno_source = NativeErrorSource::WindowsGeneric;
inline constexpr NativeErrorSource generic_source = NativeErrorSource::WindowsGeneric;
#elif defined(__APPLE__)
inline constexpr NativeErrorSource errno_source = NativeErrorSource::DarwinErrno;
inline constexpr NativeErrorSource generic_source = NativeErrorSource::DarwinGeneric;
inline constexpr NativeErrorSource resolver_source = NativeErrorSource::DarwinResolver;
#else
inline constexpr NativeErrorSource errno_source = NativeErrorSource::LinuxErrno;
inline constexpr NativeErrorSource generic_source = NativeErrorSource::LinuxGeneric;
inline constexpr NativeErrorSource resolver_source = NativeErrorSource::LinuxResolver;
#endif

// Only used at standard-library system_error boundaries (thread/service creation).
// Unknown category identity cannot be transported: keep its number under an unknown source.
inline NativeError native_error(const std::error_code& error) noexcept {
    if (!error) {
        return {};
    }

    if (error.category() == std::generic_category()) {
        return {generic_source, error.value()};
    }

    if (error.category() == std::system_category()) {
#if defined(_WIN32)
        return {NativeErrorSource::Win32, static_cast<std::uint32_t>(error.value())};
#else
        return {errno_source, error.value()};
#endif
    }

    return {static_cast<NativeErrorSource>(UINT32_MAX), error.value()};
}
}
