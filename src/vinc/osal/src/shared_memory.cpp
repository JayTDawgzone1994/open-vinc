// Common name, size and access checks for all shared-memory backends.
// Mappings retain ownership independently, so closing a handle does not invalidate a live view.

#include "shared_memory_state.hpp"
#include <limits>
#include <utility>

namespace vinc::osal {
namespace {
static_assert(sizeof(void*) == 8, "OSAL Shared Memory currently targets 64-bit processes");

OsError invalid(Operation operation) {
    return {ErrorCode::InvalidArgument, operation};
}

bool valid_access(SharedMemoryAccess access) {
    return access == SharedMemoryAccess::ReadOnly || access == SharedMemoryAccess::ReadWrite;
}

bool valid_size(std::uint64_t size) {
    return size > 0 && size <= static_cast<std::uint64_t>((std::numeric_limits<std::ptrdiff_t>::max)());
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 24) {
        return false;
    }

    for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-')) {
            return false;
        }
    }

    return true;
}
}

SharedMemory::SharedMemory() noexcept = default;
SharedMemory::~SharedMemory() = default;
SharedMemory::SharedMemory(SharedMemory&&) noexcept = default;
SharedMemory& SharedMemory::operator=(SharedMemory&&) noexcept = default;

SharedMemory::SharedMemory(std::shared_ptr<detail::SharedMemoryState> state) : state_(std::move(state)) {
}

SharedMemory::operator bool() const noexcept {
    return state_ && state_->handle != -1;
}

Result<SharedMemory> SharedMemory::create(std::string_view name, std::uint64_t size) {
    if (!valid_name(name) || !valid_size(size)) {
        return std::unexpected(invalid(Operation::CreateSharedMemory));
    }

    auto state = std::make_shared<detail::SharedMemoryState>();
    auto result = detail::create_memory(*state, std::string(name), size);
    if (!result) {
        return std::unexpected(result.error());
    }
    state->access = SharedMemoryAccess::ReadWrite;
    state->created_size = size;
    return SharedMemory(std::move(state));
}

Result<SharedMemory> SharedMemory::open(std::string_view name, SharedMemoryAccess access) {
    if (!valid_name(name) || !valid_access(access)) {
        return std::unexpected(invalid(Operation::OpenSharedMemory));
    }

    auto state = std::make_shared<detail::SharedMemoryState>();
    auto result = detail::open_memory(*state, std::string(name), access);
    if (!result) {
        return std::unexpected(result.error());
    }
    state->access = access;
    return SharedMemory(std::move(state));
}

// Validate local permissions before asking the backend to map. The mapping holds
// a shared owner reference but its virtual address is meaningful only in this process.
Result<SharedMemoryMapping> SharedMemory::map(std::uint64_t size, SharedMemoryAccess access) const {
    constexpr auto operation = Operation::MapSharedMemory;
    if (!*this) {
        return std::unexpected(OsError{ErrorCode::Closed, operation});
    }

    if (!valid_size(size) || !valid_access(access)) {
        return std::unexpected(invalid(operation));
    }

    if (state_->access == SharedMemoryAccess::ReadOnly && access == SharedMemoryAccess::ReadWrite) {
        return std::unexpected(OsError{ErrorCode::PermissionDenied, operation});
    }

    if (state_->created_size && size > state_->created_size) {
        return std::unexpected(invalid(operation));
    }

    auto mapping = std::make_unique<detail::SharedMemoryMappingState>();
    mapping->owner = state_;
    mapping->length = static_cast<std::size_t>(size);
    mapping->access = access;
    auto result = detail::map_memory(*mapping);
    if (!result) {
        return std::unexpected(result.error());
    }

    return SharedMemoryMapping(std::move(mapping));
}

Result<void> SharedMemory::close() noexcept {
    if (state_ && state_.use_count() == 1) {
        auto result = state_->close_native();
        if (!result) {
            return result;
        }
    }
    state_.reset();
    return {};
}

Result<void> SharedMemory::unlink_name(std::string_view name) {
    if (!valid_name(name)) {
        return std::unexpected(invalid(Operation::UnlinkSharedMemory));
    }

    return detail::unlink_memory(std::string(name));
}

SharedMemoryMapping::SharedMemoryMapping() noexcept = default;
SharedMemoryMapping::~SharedMemoryMapping() = default;
SharedMemoryMapping::SharedMemoryMapping(SharedMemoryMapping&&) noexcept = default;
SharedMemoryMapping& SharedMemoryMapping::operator=(SharedMemoryMapping&&) noexcept = default;

SharedMemoryMapping::SharedMemoryMapping(std::unique_ptr<detail::SharedMemoryMappingState> state)
    : state_(std::move(state)) {
}

SharedMemoryMapping::operator bool() const noexcept {
    return state_ && state_->address;
}

std::span<const std::byte> SharedMemoryMapping::bytes() const noexcept {
    return *this ? std::span<const std::byte>(state_->address, state_->length) : std::span<const std::byte>{};
}

Result<std::span<std::byte>> SharedMemoryMapping::writable_bytes() noexcept {
    if (!*this) {
        return std::unexpected(OsError{ErrorCode::Closed, Operation::MapSharedMemory});
    }

    if (state_->access != SharedMemoryAccess::ReadWrite) {
        return std::unexpected(OsError{ErrorCode::PermissionDenied, Operation::MapSharedMemory});
    }

    return std::span<std::byte>(state_->address, state_->length);
}

Result<void> SharedMemoryMapping::unmap() noexcept {
    if (state_) {
        auto result = state_->unmap_native();
        if (!result) {
            return result;
        }
        state_.reset();
    }

    return {};
}
} // namespace vinc::osal
