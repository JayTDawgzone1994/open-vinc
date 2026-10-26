#pragma once

// Private per-Publisher ring buffer format and read-lease protocol.
// Control words are writable by participants; Subscribers map the payload region read-only.

#include <vinc/message_bus/message_bus.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <atomic>
#include <vinc/detail/resource_names.hpp>
#include <functional>

namespace vinc::message_bus::detail {
using osal::shared_compare_exchange;
using osal::shared_fetch_add;
using osal::shared_load;
using osal::shared_store;
// Version-three layout and admission bounds, shared by BusManager and mapped RingBuffer validation.
inline constexpr std::uint64_t ring_buffer_layout_version = 3;
inline constexpr std::uint32_t minimum_ring_buffer_slots = 2;
inline constexpr std::uint32_t maximum_ring_buffer_slots = 65536;
inline constexpr std::uint32_t maximum_ring_buffer_readers = 4096;
inline constexpr std::uint64_t minimum_message_bytes = 64;
inline constexpr std::uint64_t payload_alignment = 8;
inline constexpr std::uint64_t payload_alignment_mask = payload_alignment - 1;
inline constexpr std::size_t control_alignment = 64;

using osal::SharedWord;

// BusManager-issued identity and dimensions. owner is a process generation; rank is
// registration order for equal timestamps. The names locate independently mapped regions.
struct Descriptor {
    std::uint64_t id = 0;
    std::uint64_t owner = 0;
    std::uint64_t schema = 0;
    std::uint64_t rank = 0;
    std::uint32_t slots = 0;
    std::uint32_t readers = 0;
    std::uint64_t bytes = 0;
    std::string control;
    std::string data;
    std::string counter;
    std::string publisher;
};

struct Attachment {
    Descriptor descriptor;
    std::uint32_t reader = 0;
    std::uint64_t floor = 0;
};

// head is the last published sequence in this Publisher ring buffer, not a topic-wide FIFO.
// generation advances on each reservation, including a write that is later abandoned.
struct alignas(control_alignment) RingBufferHeader {
    std::uint64_t magic = 0;
    std::uint64_t version = ring_buffer_layout_version;
    std::uint64_t id = 0;
    std::uint64_t schema = 0;
    std::uint64_t slots = 0;
    std::uint64_t bytes = 0;
    std::uint64_t readers = 0;
    SharedWord alive;
    SharedWord head;
    SharedWord generation;
    SharedWord collisions;
    SharedWord wait_total;
    SharedWord wait_max;
    SharedWord wait_start;
    std::uint64_t reserved[2]{};
};

// gate packs generation in the high bits and ready/draining/writing in the low two.
// Reusing a physical slot cannot recreate an old token, preventing stale reader acceptance.
struct alignas(control_alignment) Slot {
    SharedWord gate;
    SharedWord sequence;
    SharedWord timestamp;
    SharedWord count;
    SharedWord size;
    SharedWord monotonic_timestamp;
    std::uint64_t reserved[2]{};
};

// Each subscription owns one entry per ring buffer. Zero means idle; a ready gate token
// announces the exact slot generation whose bytes the framework is about to copy.
struct alignas(control_alignment) ReaderSlot {
    SharedWord token;
    // Diagnostic words never participate in the read lease or writer wait decision.
    // The owning subscriber writes them; central reads/reset only after quiescence.
    SharedWord lost_messages;
    SharedWord overwrite_events;
    SharedWord oversized_messages;
    std::uint64_t reserved[4]{};
};

static_assert(sizeof(RingBufferHeader) == 128 && sizeof(Slot) == 64 && sizeof(ReaderSlot) == 64);

struct Metadata {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t monotonic_timestamp = 0;
    std::uint64_t count = 0;
    std::uint64_t size = 0;
    std::uint64_t token = 0;
    std::uint32_t slot = 0;
};

// Merge currently visible candidates by monotonic timestamp, breaking ties by rank.
// This cannot order against an earlier timestamp whose publisher has not exposed its slot yet.
inline bool preferred(const Metadata& a, std::uint64_t a_rank, const Metadata& b, std::uint64_t b_rank,
                      bool latest) {
    if (a.monotonic_timestamp == b.monotonic_timestamp) {
        return a_rank < b_rank;
    }

    if (latest) {
        return a.monotonic_timestamp > b.monotonic_timestamp;
    }

    return a.monotonic_timestamp < b.monotonic_timestamp;
}

// Exclusive writable extent for one unpublished generation. Its span is only usable
// while the RingBuffer mapping is retained and this reservation has not been published or abandoned.
struct Reservation {
    std::uint32_t slot;
    std::uint64_t sequence;
    std::uint64_t generation;
    std::span<std::byte> bytes;
};

class RingBuffer : public std::enable_shared_from_this<RingBuffer> {
    std::shared_ptr<vinc::detail::ResourceNames> resources_;
    osal::SharedMemory control_;
    osal::SharedMemory data_;
    osal::SharedMemoryMapping control_map_;
    osal::SharedMemoryMapping data_map_;
    bool creator_ = false;
    bool writable_ = false;
    // Acquisition helpers retain each successful handle/mapping in this RingBuffer immediately.
    // On failure, the enclosing shared_ptr destroys it and unlinks only creator-owned names.
    vinc::Result<void> create_control();
    vinc::Result<void> open_control();
    vinc::Result<void> map_control();
    vinc::Result<void> initialize_control();
    vinc::Result<void> validate_control();
    vinc::Result<void> create_payload();
    vinc::Result<void> open_payload(osal::SharedMemoryAccess access);
    vinc::Result<void> map_payload(osal::SharedMemoryAccess access);

    RingBufferHeader* header_ = nullptr;
    Slot* slots_ = nullptr;
    ReaderSlot* readers_ = nullptr;

public:
    Descriptor descriptor;
    // Central-only diagnostic setting; it is not transmitted in the shared layout.
    std::uint32_t requested_slots = 0;
    ~RingBuffer();
    static std::uint64_t control_bytes(std::uint32_t slots, std::uint32_t readers);

    static std::uint64_t slot_stride(std::uint64_t bytes) {
        return (bytes + payload_alignment_mask) & ~payload_alignment_mask;
    }

    static vinc::Result<std::shared_ptr<RingBuffer>> create(Descriptor, std::shared_ptr<vinc::detail::ResourceNames> resources = {});
    static vinc::Result<std::shared_ptr<RingBuffer>> open(Descriptor, bool writer);

    RingBufferHeader& header() {
        return *header_;
    }

    Slot& slot(std::uint32_t i) {
        return slots_[i];
    }

    ReaderSlot& reader(std::uint32_t i) {
        return readers_[i];
    }

    void retire() noexcept;
    bool alive() const noexcept;
    void clear_reader(std::uint32_t) noexcept; // owner releases its own lease, or central confirms quiescence
    void reset_reader(std::uint32_t) noexcept; // central only: quiescent unsubscribe or confirmed death
    vinc::Result<Reservation> reserve(const std::atomic<bool>& closed);
    vinc::Result<void> publish(const Reservation&, std::uint64_t image_size, std::uint64_t timestamp,
                         std::uint64_t monotonic_timestamp, std::uint64_t count);
    void abandon(const Reservation&) noexcept;
    std::optional<Metadata> candidate(std::uint64_t cursor, bool latest, std::uint64_t upper = UINT64_MAX);
    bool protect(const Metadata&, std::uint32_t reader);
    void unprotect(std::uint32_t reader) noexcept;
    std::span<const std::byte> payload(const Metadata&) const;
    PublisherStats stats(std::uint32_t requested);
};

struct NamedCounter {
    std::shared_ptr<vinc::detail::ResourceNames> resources;
    osal::SharedMemory memory;
    osal::SharedMemoryMapping mapping;
    std::string name;
    bool creator = false;
    ~NamedCounter();
    static vinc::Result<std::shared_ptr<NamedCounter>> create(std::string, std::shared_ptr<vinc::detail::ResourceNames> resources = {});
    static vinc::Result<std::shared_ptr<NamedCounter>> open(std::string);
    vinc::Result<std::uint64_t> next();
};

std::string shared_name(char suffix);
vinc::Error storage_error(const vinc::Error&, ErrorOperation) noexcept;
}
