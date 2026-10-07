#include <vinc/osal/detail/native_error.hpp>
#include <vinc/osal/detail/posix/descriptor.hpp>
// POSIX shm_open/mmap backend with exclusive creation and owner-only initial permissions.
// Names and mappings have separate lifetimes: unlink removes discovery, not existing views.

#include <vinc/osal/detail/shared_memory_state.hpp>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vinc::osal::detail {
namespace {
vinc::Error error(vinc::osal::ErrorOperation operation, int value = errno) noexcept {
    vinc::osal::ErrorCode code = vinc::osal::ErrorCode::SystemError;
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
    case ENOMEM:
    case ENOSPC:
    case EMFILE:
    case ENFILE:
        code = vinc::osal::ErrorCode::ResourceExhausted;
        break;
    case EINVAL:
    case EFBIG:
    case EOVERFLOW:
        code = vinc::osal::ErrorCode::InvalidArgument;
        break;
    }

    return {code, operation, {detail::errno_source, value}};
}

std::string native_name(const std::string& name) {
    return "/ov_" + name;
}
}

SharedMemoryState::~SharedMemoryState() {
    (void)close_native();
}

vinc::Result<void> SharedMemoryState::close_native() noexcept {
    if (handle == -1) {
        return {};
    }
    // Never retry close after EINTR: a descriptor number may already be reused.
    const int fd = static_cast<int>(std::exchange(handle, -1));
    if (::close(fd) != 0) {
        return std::unexpected(error(vinc::osal::ErrorOperation::CloseSharedMemory));
    }

    return {};
}

SharedMemoryMappingState::~SharedMemoryMappingState() {
    (void)unmap_native();
}

vinc::Result<void> SharedMemoryMappingState::unmap_native() noexcept {
    if (!address) {
        return {};
    }

    if (::munmap(address, length) != 0) {
        return std::unexpected(error(vinc::osal::ErrorOperation::UnmapSharedMemory));
    }
    address = nullptr;
    length = 0;
    return {};
}

vinc::Result<void> create_memory(SharedMemoryState& state, const std::string& name, std::uint64_t size) {
    constexpr auto operation = vinc::osal::ErrorOperation::CreateSharedMemory;
    if (size > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation});
    }

    const auto native = native_name(name);
    int fd;
    do {
        fd = ::shm_open(native.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return std::unexpected(error(operation));
    }
    auto isolated = isolate_descriptor(fd, operation);
    if (!isolated) {
        (void)::close(fd);
        (void)::shm_unlink(native.c_str()); // only the unpublished object we created
        return std::unexpected(isolated.error());
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

vinc::Result<void> open_memory(SharedMemoryState& state, const std::string& name, SharedMemoryAccess access) {
    const auto native = native_name(name);
    int fd;
    do {
        fd = ::shm_open(native.c_str(), access == SharedMemoryAccess::ReadWrite ? O_RDWR : O_RDONLY, 0);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return std::unexpected(error(vinc::osal::ErrorOperation::OpenSharedMemory));
    }
    auto isolated = isolate_descriptor(fd, ErrorOperation::OpenSharedMemory);
    if (!isolated) {
        (void)::close(fd);
        return std::unexpected(isolated.error());
    }

    state.handle = fd;
    return {};
}

vinc::Result<void> map_memory(SharedMemoryMappingState& state) {
    constexpr auto operation = vinc::osal::ErrorOperation::MapSharedMemory;
    struct stat info{};
    int result;
    do {
        result = ::fstat(static_cast<int>(state.owner->handle), &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return std::unexpected(error(operation));
    }

    if (info.st_size < 0 || state.length > static_cast<std::uint64_t>(info.st_size)) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation});
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

vinc::Result<void> unlink_memory(const std::string& name) {
    const auto native = native_name(name);
    if (::shm_unlink(native.c_str()) != 0) {
        return std::unexpected(error(vinc::osal::ErrorOperation::UnlinkSharedMemory));
    }

    return {};
}
} // namespace vinc::osal::detail
