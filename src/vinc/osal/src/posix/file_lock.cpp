#include <vinc/osal/detail/native_error.hpp>
#include <vinc/osal/detail/posix/descriptor.hpp>
// Every process opens its own file description. No dup/fork handoff: inherited
// references could keep a dead owner's lock alive. This is cooperative locking.
#include <vinc/osal/detail/file_lock_state.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vinc::osal::detail {
namespace {
vinc::Error error(vinc::osal::ErrorOperation operation, int value = errno) {
    auto code = vinc::osal::ErrorCode::SystemError;
    switch (value) {
    case EEXIST:
        code = vinc::osal::ErrorCode::AlreadyExists;
        break;
    case ENOENT:
        code = vinc::osal::ErrorCode::NotFound;
        break;
    case EACCES:
    case EPERM:
        code = vinc::osal::ErrorCode::PermissionDenied;
        break;
    case EAGAIN:
        code = vinc::osal::ErrorCode::WouldBlock;
        break;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
        code = vinc::osal::ErrorCode::WouldBlock;
        break;
#endif
    case ENOMEM:
    case EMFILE:
    case ENFILE:
    case ENOSPC:
    case ENOLCK:
        code = vinc::osal::ErrorCode::ResourceExhausted;
        break;
    case EINVAL:
    case ELOOP:
    case ENOTDIR:
    case ENAMETOOLONG:
        code = vinc::osal::ErrorCode::InvalidArgument;
        break;
    case ENOTSUP:
        code = vinc::osal::ErrorCode::Unsupported;
        break;
    }

    return {code, operation, vinc::NativeError{detail::errno_source, value}};
}
}

FileLockState::~FileLockState() {
    if (handle != -1) {
        (void)::close(static_cast<int>(handle));
    }
}

vinc::Result<void> open_file_lock(FileLockState& state, const std::filesystem::path& path, bool create) {
    const auto operation =
        create ? vinc::osal::ErrorOperation::CreateFileLock : vinc::osal::ErrorOperation::OpenFileLock;
    const int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_CREAT | O_EXCL : 0);
    int fd;
    do {
        fd = ::open(path.c_str(), flags, 0600);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1) {
        return std::unexpected(error(operation));
    }
    auto isolated = isolate_descriptor(fd, operation);
    if (!isolated) {
        (void)::close(fd);
        if (create) {
            // The new name has not been published. Never unlink an opened resource.
            (void)::unlink(path.c_str());
        }

        return std::unexpected(isolated.error());
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        auto failure = error(operation);
        (void)::close(fd);
        return std::unexpected(failure);
    }
    // Parent-directory privacy and stable paths remain caller responsibilities.
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
        (void)::close(fd);
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation});
    }
    state.handle = fd;
    return {};
}

vinc::Result<void> try_lock_file_lock(FileLockState& state) {
    if (::flock(static_cast<int>(state.handle), LOCK_EX | LOCK_NB) == 0) {
        return {};
    }
    // Return to the common loop on interruption so deadlines are checked again.
    if (errno == EINTR) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::WouldBlock, vinc::osal::ErrorOperation::LockFileLock});
    }

    return std::unexpected(error(vinc::osal::ErrorOperation::LockFileLock));
}

vinc::Result<void> unlock_file_lock(FileLockState& state) {
    int result;
    do {
        result = ::flock(static_cast<int>(state.handle), LOCK_UN);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return {};
    }

    return std::unexpected(error(vinc::osal::ErrorOperation::UnlockFileLock));
}
} // namespace vinc::osal::detail
