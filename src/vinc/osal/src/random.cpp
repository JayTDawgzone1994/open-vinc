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
namespace {
// getentropy accepts at most 256 bytes per call; use the same chunk on each backend.
constexpr std::size_t max_random_chunk_bytes = 256;
}

Result<void> random_bytes(std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto chunk_size = (std::min)(bytes.size(), max_random_chunk_bytes);
#ifdef _WIN32
        const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                                            static_cast<ULONG>(chunk_size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return std::unexpected(
                OsError{ErrorCode::SystemError, Operation::RandomBytes,
                        std::error_code(static_cast<int>(status), std::system_category())});
        }
#else
        if (::getentropy(bytes.data(), chunk_size) != 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected(OsError{ErrorCode::SystemError, Operation::RandomBytes,
                                           std::error_code(errno, std::generic_category())});
        }
#endif
        bytes = bytes.subspan(chunk_size);
    }

    return {};
}
}
