#pragma once
#include <vinc/param/detail/protocol.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/detail/resource_names.hpp>

namespace vinc::param::detail {
// Byte-encoded A/B images. Only the shared mutex selects the published slot.
inline constexpr std::size_t slot_header_bytes = 16;
inline constexpr std::size_t slot_extent = slot_header_bytes + config::param_slot_bytes;
inline constexpr std::size_t storage_bytes = 2 * slot_extent;

struct StoreDescriptor {
    std::uint64_t lock_id = 0;
    std::string data_name;
};

class Storage {
public:
    static Result<std::unique_ptr<Storage>> create(std::shared_ptr<vinc::detail::ResourceNames> names,
                                                   std::string_view lock_name, std::uint64_t lock_id);
    ~Storage();

    const StoreDescriptor& descriptor() const noexcept {
        return descriptor_;
    }

    Result<void> publish(const Snapshot& snapshot, std::stop_token stop = {});

private:
    std::shared_ptr<vinc::detail::ResourceNames> names_;
    StoreDescriptor descriptor_;
    osal::SharedMemory memory_;
    osal::SharedMemoryMapping mapping_;
    concurrency::InterprocessMutex mutex_;
    bool created_ = false;
    Result<void> open_mutex(std::string_view name);
    Result<void> create_image();
};

// Caller owns the Host lease and invokes this only while holding the shared lock.
// Buffer capacity is prepared before locking; decode happens after unlocking.
Result<std::uint64_t> copy_image(std::span<const std::byte> mapping, bool publication,
                                 std::uint64_t known_revision, control::Bytes& buffer);
}
