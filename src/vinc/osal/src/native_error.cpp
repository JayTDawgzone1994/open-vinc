#include <vinc/osal/detail/error_catalog.hpp>
#include <vinc/osal/detail/native_error.hpp>
#include <bit>
#include <limits>
#include <system_error>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace vinc::osal::detail {
std::string native_error_text(const NativeError& error) {
    // Foreign source namespaces must never be interpreted using the host's tables.
#if defined(_WIN32)
    if (error.source == NativeErrorSource::Win32 || error.source == NativeErrorSource::Winsock) {
        if (error.code < 0 || error.code > (std::numeric_limits<std::uint32_t>::max)()) {
            return {};
        }

        // std::error_code accepts int; preserve the Win32 32-bit pattern at this ABI boundary.
        const auto value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(error.code));
        return std::system_category().message(value);
    }
    // NTSTATUS has a different namespace. Retain its identity/number rather than
    // asking the Win32 formatter to interpret it or loading extra native modules.
#endif
    if (error.code < (std::numeric_limits<int>::min)() || error.code > (std::numeric_limits<int>::max)()) {
        return {};
    }

    const auto value = static_cast<int>(error.code);
    if (error.source == errno_source || error.source == generic_source) {
        return std::generic_category().message(value);
    }

#if !defined(_WIN32)
    if (error.source == resolver_source) {
        const auto* text = gai_strerror(value);
        return text ? std::string(text) : std::string{};
    }
#endif
    return {};
}
}

namespace vinc::osal {
NativeError native_error(const std::error_code& error) noexcept {
    return detail::native_error(error);
}
}
