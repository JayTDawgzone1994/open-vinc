#pragma once

// Internal descriptors must not fill a missing standard stream: spawn explicitly
// preserves inherited stdio even when its original descriptor has FD_CLOEXEC.
#include <vinc/osal/error.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace vinc::osal::detail {
vinc::Error native_error(ErrorOperation operation, int value) noexcept;

// The caller owns fd throughout. Failure leaves it unchanged for caller cleanup;
// success replaces a low descriptor and closes the old one exactly once.
inline vinc::Result<void> isolate_descriptor(int& fd, ErrorOperation operation) noexcept {
    constexpr int first_non_stdio_descriptor = STDERR_FILENO + 1;
    if (fd >= first_non_stdio_descriptor) {
        return {};
    }

    int replacement;
    do {
        replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, first_non_stdio_descriptor);
    } while (replacement < 0 && errno == EINTR);
    if (replacement < 0) {
        return std::unexpected(native_error(operation, errno));
    }

    // Do not retry close after EINTR: the old number may already be reused.
    (void)::close(fd);
    fd = replacement;
    return {};
}
} // namespace vinc::osal::detail
