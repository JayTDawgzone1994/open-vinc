#pragma once
#include <vinc/bus/bus.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <atomic>
#include <functional>

namespace vinc::bus::detail {
using osal::SharedWord;
using osal::shared_load; using osal::shared_store; using osal::shared_fetch_add; using osal::shared_compare_exchange;
struct Descriptor {
    std::uint64_t id = 0, owner = 0, schema = 0, rank = 0;
    std::uint32_t slots = 0, readers = 0;
    std::uint64_t bytes = 0;
    std::string control, data, counter, publisher;
};
struct Attachment { Descriptor descriptor; std::uint32_t reader = 0; std::uint64_t floor = 0; };
struct alignas(64) RingHeader {
    std::uint64_t magic = 0, version = 1, id = 0, schema = 0, slots = 0, bytes = 0, readers = 0;
    SharedWord alive, head, generation, collisions, wait_total, wait_max, wait_start;
    std::uint64_t reserved[2]{};
};
struct alignas(64) Slot { SharedWord gate, sequence, timestamp, count, size; std::uint64_t reserved[3]{}; };
struct alignas(64) ReaderSlot { SharedWord token; std::uint64_t reserved[7]{}; };
static_assert(sizeof(RingHeader) == 128 && sizeof(Slot) == 64 && sizeof(ReaderSlot) == 64);
struct Metadata { std::uint64_t sequence = 0, timestamp = 0, count = 0, size = 0, token = 0; std::uint32_t slot = 0; };
inline bool preferred(const Metadata& a, std::uint64_t a_rank, const Metadata& b, std::uint64_t b_rank, bool latest) {
    return a.timestamp == b.timestamp ? a_rank < b_rank : (latest ? a.timestamp > b.timestamp : a.timestamp < b.timestamp);
}
struct Reservation { std::uint32_t slot; std::uint64_t sequence, generation; std::span<std::byte> bytes; };
class Ring : public std::enable_shared_from_this<Ring> {
    osal::SharedMemory control_, data_;
    osal::SharedMemoryMapping control_map_, data_map_;
    bool creator_ = false, writable_ = false;
    RingHeader* header_ = nullptr;
    Slot* slots_ = nullptr;
    ReaderSlot* readers_ = nullptr;
public:
    Descriptor descriptor;
    ~Ring();
    static std::uint64_t control_bytes(std::uint32_t slots, std::uint32_t readers);
    static std::uint64_t slot_stride(std::uint64_t bytes) { return (bytes + 7) & ~std::uint64_t{7}; }
    static Result<std::shared_ptr<Ring>> create(Descriptor);
    static Result<std::shared_ptr<Ring>> open(Descriptor, bool writer);
    RingHeader& header() { return *header_; }
    Slot& slot(std::uint32_t i) { return slots_[i]; }
    ReaderSlot& reader(std::uint32_t i) { return readers_[i]; }
    void retire() noexcept;
    bool alive() const noexcept;
    void clear_reader(std::uint32_t) noexcept; // only quiescent unsubscribe or confirmed process death
    Result<Reservation> reserve(const std::atomic<bool>& closed);
    Result<void> publish(const Reservation&, std::uint64_t image_size, std::uint64_t timestamp, std::uint64_t count);
    void abandon(const Reservation&) noexcept;
    std::optional<Metadata> candidate(std::uint64_t cursor, bool latest);
    bool protect(const Metadata&, std::uint32_t reader);
    void unprotect(std::uint32_t reader) noexcept;
    std::span<const std::byte> payload(const Metadata&) const;
    PublisherStats stats(std::uint32_t requested);
};
struct NamedCounter {
    osal::SharedMemory memory;
    osal::SharedMemoryMapping mapping;
    std::string name;
    bool creator = false;
    ~NamedCounter();
    static Result<std::shared_ptr<NamedCounter>> create(std::string);
    static Result<std::shared_ptr<NamedCounter>> open(std::string);
    Result<std::uint64_t> next();
};
std::string shared_name(char suffix);
Error error(ErrorCode, std::string);
Error storage_error(message::Error);
}
