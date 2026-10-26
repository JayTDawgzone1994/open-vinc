#include <vinc/osal/detail/native_error.hpp>
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

vinc::Result<void> random_bytes(std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto chunk_size = (std::min)(bytes.size(), max_random_chunk_bytes);
#ifdef _WIN32
        // UCHAR is unsigned char: it may write the byte span's object
        // representation without extra alignment. chunk_size bounds the write.
        const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                                            static_cast<ULONG>(chunk_size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return std::unexpected(vinc::Error{
                vinc::osal::ErrorCode::SystemError, vinc::osal::ErrorOperation::RandomBytes,
                vinc::NativeError{vinc::NativeErrorSource::NtStatus, static_cast<std::uint32_t>(status)}});
        }
#else
        if (::getentropy(bytes.data(), chunk_size) != 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected(vinc::Error{vinc::osal::ErrorCode::SystemError,
                                               vinc::osal::ErrorOperation::RandomBytes,
                                               vinc::NativeError{detail::errno_source, errno}});
        }
#endif
        bytes = bytes.subspan(chunk_size);
    }

    return {};
}
}
