#include <vinc/osal/random.hpp>
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace vinc::osal {
Result<void> random_bytes(std::span<std::byte> bytes) {
    // Small chunks also satisfy the 256-byte getentropy limit on Linux/macOS.
    while (!bytes.empty()) {
        const auto n = (std::min)(bytes.size(), std::size_t{256});
#ifdef _WIN32
        const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) return std::unexpected(OsError{ErrorCode::SystemError, Operation::RandomBytes,
            std::error_code(static_cast<int>(status), std::system_category())});
#else
        if (::getentropy(bytes.data(), n) != 0) {
            if (errno == EINTR) continue;
            return std::unexpected(OsError{ErrorCode::SystemError, Operation::RandomBytes,
                std::error_code(errno, std::generic_category())});
        }
#endif
        bytes = bytes.subspan(n);
    }
    return {};
}
}
