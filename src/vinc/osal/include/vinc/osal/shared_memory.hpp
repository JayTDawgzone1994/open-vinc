#pragma once
#include <vinc/osal/error.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace vinc::osal {
enum class SharedMemoryAccess { ReadOnly, ReadWrite };
namespace detail { struct SharedMemoryState; struct SharedMemoryMappingState; }

// Owns a local mapping. All borrowed spans expire on unmap/destruction/move
// assignment that replaces this mapping. This class supplies no synchronization.
class SharedMemoryMapping {
public:
    SharedMemoryMapping() noexcept;
    ~SharedMemoryMapping();
    SharedMemoryMapping(SharedMemoryMapping&&) noexcept;
    SharedMemoryMapping& operator=(SharedMemoryMapping&&) noexcept;
    SharedMemoryMapping(const SharedMemoryMapping&) = delete;
    SharedMemoryMapping& operator=(const SharedMemoryMapping&) = delete;
    explicit operator bool() const noexcept;
    std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] Result<std::span<std::byte>> writable_bytes() noexcept;
    [[nodiscard]] Result<void> unmap() noexcept; // idempotent
private:
    friend class SharedMemory;
    explicit SharedMemoryMapping(std::unique_ptr<detail::SharedMemoryMappingState>);
    std::unique_ptr<detail::SharedMemoryMappingState> state_;
};

class SharedMemory {
public:
    SharedMemory() noexcept;
    ~SharedMemory(); // releases local ownership; never unlinks a POSIX name
    SharedMemory(SharedMemory&&) noexcept;
    SharedMemory& operator=(SharedMemory&&) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    explicit operator bool() const noexcept;

    // Logical names: 1..24 ASCII letters/digits/'_'/'-'. OSAL adds its namespace.
    // Exclusive creation, zero-filled storage, fixed positive size, no resize.
    [[nodiscard]] static Result<SharedMemory> create(std::string_view name, std::uint64_t byte_size);
    [[nodiscard]] static Result<SharedMemory> open(std::string_view name, SharedMemoryAccess);
    // Prefix mapping from offset zero. No copy-on-write or executable views.
    // byte_size is a requested extent, not a query of the creator's logical size.
    [[nodiscard]] Result<SharedMemoryMapping> map(std::uint64_t byte_size, SharedMemoryAccess) const;
    // Existing mappings retain the underlying resource independently.
    [[nodiscard]] Result<void> close() noexcept;
    // POSIX: removes the name, not existing mappings. NotFound if absent.
    // Windows: Unsupported. Use unique names; only the designated owner unlinks.
    [[nodiscard]] static Result<void> unlink_name(std::string_view name);
private:
    explicit SharedMemory(std::shared_ptr<detail::SharedMemoryState>);
    std::shared_ptr<detail::SharedMemoryState> state_;
};
} // namespace vinc::osal
