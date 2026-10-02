// POSIX shm_open/mmap backend with exclusive creation and owner-only initial permissions.
// Names and mappings have separate lifetimes: unlink removes discovery, not existing views.

#include "../shared_memory_state.hpp"
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vinc::osal::detail {
namespace {
OsError error(Operation operation, int value = errno) noexcept {
    ErrorCode code = ErrorCode::SystemError;
    switch (value) {
    case EEXIST:
        code = ErrorCode::AlreadyExists;
        break;
    case ENOENT:
        code = ErrorCode::NotFound;
        break;
    case EACCES:
    case EPERM:
        code = ErrorCode::PermissionDenied;
        break;
    case ENOMEM:
    case ENOSPC:
    case EMFILE:
    case ENFILE:
        code = ErrorCode::ResourceExhausted;
        break;
    case EINVAL:
    case EFBIG:
    case EOVERFLOW:
        code = ErrorCode::InvalidArgument;
        break;
    }

    return {code, operation, {value, std::generic_category()}};
}

std::string native_name(const std::string& name) {
    return "/ov_" + name;
}
}

SharedMemoryState::~SharedMemoryState() {
    (void)close_native();
}

Result<void> SharedMemoryState::close_native() noexcept {
    if (handle == -1) {
        return {};
    }
    // Never retry close after EINTR: a descriptor number may already be reused.
    const int fd = static_cast<int>(std::exchange(handle, -1));
    if (::close(fd) != 0) {
        return std::unexpected(error(Operation::CloseSharedMemory));
    }

    return {};
}

SharedMemoryMappingState::~SharedMemoryMappingState() {
    (void)unmap_native();
}

Result<void> SharedMemoryMappingState::unmap_native() noexcept {
    if (!address) {
        return {};
    }

    if (::munmap(address, length) != 0) {
        return std::unexpected(error(Operation::UnmapSharedMemory));
    }
    address = nullptr;
    length = 0;
    return {};
}

Result<void> create_memory(SharedMemoryState& state, const std::string& name, std::uint64_t size) {
    constexpr auto operation = Operation::CreateSharedMemory;
    if (size > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
    }

    const auto native = native_name(name);
    int fd;
    do {
        fd = ::shm_open(native.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return std::unexpected(error(operation));
    }
    state.handle = fd; // RAII closes on any subsequent failure
    int result;
    do {
        result = ::ftruncate(fd, static_cast<off_t>(size));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        const auto failure = error(operation);
        (void)::shm_unlink(native.c_str()); // remove only the object we just created
        return std::unexpected(failure);
    }
    // shm_open specifies FD_CLOEXEC; no inherited descriptor protocol is used.
    return {};
}

Result<void> open_memory(SharedMemoryState& state, const std::string& name, SharedMemoryAccess access) {
    const auto native = native_name(name);
    int fd;
    do {
        fd = ::shm_open(native.c_str(), access == SharedMemoryAccess::ReadWrite ? O_RDWR : O_RDONLY, 0);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return std::unexpected(error(Operation::OpenSharedMemory));
    }
    state.handle = fd;
    return {};
}

Result<void> map_memory(SharedMemoryMappingState& state) {
    constexpr auto operation = Operation::MapSharedMemory;
    struct stat info{};
    int result;
    do {
        result = ::fstat(static_cast<int>(state.owner->handle), &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return std::unexpected(error(operation));
    }

    if (info.st_size < 0 || state.length > static_cast<std::uint64_t>(info.st_size)) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
    }

    const int protection = PROT_READ | (state.access == SharedMemoryAccess::ReadWrite ? PROT_WRITE : 0);
    void* address =
        ::mmap(nullptr, state.length, protection, MAP_SHARED, static_cast<int>(state.owner->handle), 0);
    if (address == MAP_FAILED) {
        return std::unexpected(error(operation));
    }
    state.address = static_cast<std::byte*>(address);
    return {};
}

Result<void> unlink_memory(const std::string& name) {
    const auto native = native_name(name);
    if (::shm_unlink(native.c_str()) != 0) {
        return std::unexpected(error(Operation::UnlinkSharedMemory));
    }

    return {};
}
} // namespace vinc::osal::detail
