// RingBuffer publication and overwrite protection using generation-tagged shared atomic words.
// Atomic metadata selects a complete payload; the payload bytes themselves are not atomic.

#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <thread>

namespace vinc::message_bus::detail {
namespace {
constexpr std::uint64_t magic = 0x31474e495242564fULL;
// Slot lifecycle: empty -> writing -> ready -> draining -> writing.
// Draining blocks new readers before the writer waits for old-generation leases to clear.
constexpr unsigned gate_state_bits = 2;
constexpr std::uint64_t gate_state_mask = 3;
constexpr std::uint64_t maximum_generation = (UINT64_MAX >> gate_state_bits) - 1;
constexpr std::uint64_t ready = 1;
constexpr std::uint64_t draining = 2;
constexpr std::uint64_t writing = 3;

vinc::Error io(const vinc::Error& failure, ErrorOperation operation) {
    return vinc::wrap_error(ErrorCode::Io, operation, failure);
}

void unlink(const std::string& name, const std::shared_ptr<vinc::detail::ResourceNames>& resources = {}) noexcept {
    if (!name.empty()) {
        try {
            auto unlinked = osal::SharedMemory::unlink_name(name);
            if (resources && (unlinked || unlinked.error().is(osal::ErrorCode::NotFound) ||
                              unlinked.error().is(osal::ErrorCode::Unsupported))) {
                (void)resources->retire(name);
            }
        } catch (...) {
            // Keep the persisted record when cleanup cannot finish during unwinding.
        }
    }
}

std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
}

// Storage owns validation/capacity diagnostics; this layer exposes Bus classifications.
vinc::Error storage_error(const vinc::Error& failure, ErrorOperation operation) noexcept {
    auto code = ErrorCode::InvalidData;
    if (failure.is(message::ErrorCode::CapacityExceeded)) {
        code = ErrorCode::CapacityExceeded;
    } else if (failure.is(message::ErrorCode::ResourceExhausted)) {
        code = ErrorCode::ResourceExhausted;
    } else if (failure.is(message::ErrorCode::TypeMismatch)) {
        code = ErrorCode::TypeMismatch;
    } else if (failure.is(message::ErrorCode::Closed)) {
        code = ErrorCode::Closed;
    }

    return vinc::wrap_error(code, operation, failure);
}

std::string shared_name(char suffix) {
    std::random_device random;
    constexpr int random_words = 2;
    constexpr int highest_nibble_shift = 28;
    constexpr int nibble_bits = 4;
    constexpr unsigned nibble_mask = 15;
    constexpr char hex[] = "0123456789abcdef";
    std::string name = "vb";
    for (int i = 0; i < random_words; ++i) {
        const auto word = random();
        for (int shift = highest_nibble_shift; shift >= 0; shift -= nibble_bits) {
            name += hex[(word >> shift) & nibble_mask];
        }
    }
    name += suffix;
    return name;
}

std::uint64_t RingBuffer::control_bytes(std::uint32_t slots, std::uint32_t readers) {
    return sizeof(RingBufferHeader) + std::uint64_t(slots) * sizeof(Slot) +
           std::uint64_t(readers) * sizeof(ReaderSlot);
}

RingBuffer::~RingBuffer() {
    if (creator_) {
        retire();
        if (control_) {
            unlink(descriptor.control, resources_);
        }

        if (data_) {
            unlink(descriptor.data, resources_);
        }
    }
}

vinc::Result<void> RingBuffer::create_control() {
    auto memory =
        osal::SharedMemory::create(descriptor.control, control_bytes(descriptor.slots, descriptor.readers));
    if (!memory) {
        creator_ = false;
        return std::unexpected(io(memory.error(), ErrorOperation::CreateControl));
    }
    control_ = std::move(*memory);
    return {};
}

vinc::Result<void> RingBuffer::map_control() {
    auto mapping = control_.map(control_bytes(descriptor.slots, descriptor.readers),
                                osal::SharedMemoryAccess::ReadWrite);
    if (!mapping) {
        return std::unexpected(io(mapping.error(), ErrorOperation::MapControl));
    }
    control_map_ = std::move(*mapping);
    return {};
}

vinc::Result<void> RingBuffer::initialize_control() {
    auto writable = control_map_.writable_bytes();
    if (!writable) {
        return std::unexpected(io(writable.error(), ErrorOperation::InitializeControl));
    }
    header_ = ::new (writable->data()) RingBufferHeader{};
    header_->magic = magic;
    header_->id = descriptor.id;
    header_->schema = descriptor.schema;
    header_->slots = descriptor.slots;
    header_->bytes = descriptor.bytes;
    header_->readers = descriptor.readers;
    // Shared ABI boundary: OS mappings are page-aligned; the 128-byte header and
    // 64-byte slot strides preserve alignment. The creator starts these trivial
    // objects' lifetimes below; open attaches to that same-version native layout.
    slots_ = reinterpret_cast<Slot*>(writable->data() + sizeof(RingBufferHeader));
    for (std::uint32_t i = 0; i < descriptor.slots; ++i) {
        ::new (slots_ + i) Slot{};
    }
    readers_ = reinterpret_cast<ReaderSlot*>(writable->data() + sizeof(RingBufferHeader) +
                                             descriptor.slots * sizeof(Slot));
    for (std::uint32_t i = 0; i < descriptor.readers; ++i) {
        ::new (readers_ + i) ReaderSlot{};
    }

    return {};
}

vinc::Result<void> RingBuffer::create_payload() {
    auto data = osal::SharedMemory::create(descriptor.data, descriptor.slots * slot_stride(descriptor.bytes));
    if (!data) {
        descriptor.data.clear();
        return std::unexpected(io(data.error(), ErrorOperation::CreatePayload));
    }
    data_ = std::move(*data);
    return {};
}

vinc::Result<void> RingBuffer::open_control() {
    auto memory = osal::SharedMemory::open(descriptor.control, osal::SharedMemoryAccess::ReadWrite);
    if (!memory) {
        return std::unexpected(io(memory.error(), ErrorOperation::OpenControl));
    }
    control_ = std::move(*memory);
    return {};
}

vinc::Result<void> RingBuffer::validate_control() {
    auto writable = control_map_.writable_bytes();
    if (!writable) {
        return std::unexpected(io(writable.error(), ErrorOperation::ValidateControl));
    }
    // Native shared-memory ABI: the creator initialized this trivial header before
    // advertising the mapping. OS page alignment satisfies its 64-byte alignment.
    header_ = reinterpret_cast<RingBufferHeader*>(writable->data());
    const auto& header = *header_;
    if (header.magic != magic || header.version != ring_buffer_layout_version || header.id != descriptor.id ||
        header.schema != descriptor.schema || header.slots != descriptor.slots ||
        header.bytes != descriptor.bytes || header.readers != descriptor.readers) {
        return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::ValidateControl));
    }
    // Shared ABI boundary: OS mappings are page-aligned; the 128-byte header and
    // 64-byte slot strides preserve alignment. Objects were initialized by the
    // creator; attach requires the same native layout and OSAL shared-atomic ABI.
    slots_ = reinterpret_cast<Slot*>(writable->data() + sizeof(RingBufferHeader));
    readers_ = reinterpret_cast<ReaderSlot*>(writable->data() + sizeof(RingBufferHeader) +
                                             header.slots * sizeof(Slot));
    return {};
}

vinc::Result<void> RingBuffer::open_payload(osal::SharedMemoryAccess access) {
    auto data = osal::SharedMemory::open(descriptor.data, access);
    if (!data) {
        return std::unexpected(io(data.error(), ErrorOperation::OpenPayload));
    }
    data_ = std::move(*data);
    return {};
}

vinc::Result<void> RingBuffer::map_payload(osal::SharedMemoryAccess access) {
    auto mapping = data_.map(descriptor.slots * slot_stride(descriptor.bytes), access);
    if (!mapping) {
        return std::unexpected(io(mapping.error(), ErrorOperation::MapPayload));
    }
    data_map_ = std::move(*mapping);
    return {};
}

vinc::Result<std::shared_ptr<RingBuffer>> RingBuffer::create(Descriptor descriptor, std::shared_ptr<vinc::detail::ResourceNames> resources) {
    if (descriptor.slots < minimum_ring_buffer_slots || descriptor.slots > maximum_ring_buffer_slots ||
        !descriptor.readers || descriptor.readers > maximum_ring_buffer_readers ||
        descriptor.bytes < minimum_message_bytes || descriptor.bytes > SIZE_MAX - payload_alignment_mask ||
        slot_stride(descriptor.bytes) > SIZE_MAX / descriptor.slots) {
        return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::CreateRingBuffer));
    }

    auto ring_buffer = std::make_shared<RingBuffer>();
    ring_buffer->descriptor = std::move(descriptor);
    ring_buffer->resources_ = std::move(resources);
    ring_buffer->creator_ = true;
    ring_buffer->writable_ = true;

    auto create_control_result = ring_buffer->create_control();
    if (!create_control_result) {
        return std::unexpected(create_control_result.error());
    }

    auto map_control_result = ring_buffer->map_control();
    if (!map_control_result) {
        return std::unexpected(map_control_result.error());
    }

    auto initialize_control_result = ring_buffer->initialize_control();
    if (!initialize_control_result) {
        return std::unexpected(initialize_control_result.error());
    }

    auto create_payload_result = ring_buffer->create_payload();
    if (!create_payload_result) {
        return std::unexpected(create_payload_result.error());
    }

    auto map_payload_result = ring_buffer->map_payload(osal::SharedMemoryAccess::ReadWrite);
    if (!map_payload_result) {
        return std::unexpected(map_payload_result.error());
    }

    shared_store(ring_buffer->header_->alive, 1);
    return ring_buffer;
}

vinc::Result<std::shared_ptr<RingBuffer>> RingBuffer::open(Descriptor descriptor, bool writer) {
    if (descriptor.slots < minimum_ring_buffer_slots || !descriptor.readers ||
        descriptor.slots > maximum_ring_buffer_slots || descriptor.readers > maximum_ring_buffer_readers ||
        descriptor.bytes < minimum_message_bytes || descriptor.bytes > SIZE_MAX - payload_alignment_mask ||
        slot_stride(descriptor.bytes) > SIZE_MAX / descriptor.slots) {
        return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::OpenRingBuffer));
    }

    auto ring_buffer = std::make_shared<RingBuffer>();
    ring_buffer->descriptor = std::move(descriptor);
    ring_buffer->writable_ = writer;

    auto open_control_result = ring_buffer->open_control();
    if (!open_control_result) {
        return std::unexpected(open_control_result.error());
    }

    auto map_control_result = ring_buffer->map_control();
    if (!map_control_result) {
        return std::unexpected(map_control_result.error());
    }

    auto validate_control_result = ring_buffer->validate_control();
    if (!validate_control_result) {
        return std::unexpected(validate_control_result.error());
    }

    const auto access = writer ? osal::SharedMemoryAccess::ReadWrite : osal::SharedMemoryAccess::ReadOnly;
    auto open_payload_result = ring_buffer->open_payload(access);
    if (!open_payload_result) {
        return std::unexpected(open_payload_result.error());
    }

    auto map_payload_result = ring_buffer->map_payload(access);
    if (!map_payload_result) {
        return std::unexpected(map_payload_result.error());
    }

    if (!ring_buffer->alive()) {
        return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::OpenRingBuffer));
    }

    return ring_buffer;
}

void RingBuffer::retire() noexcept {
    if (header_) {
        shared_store(header_->alive, 0);
    }
}

bool RingBuffer::alive() const noexcept {
    return header_ && shared_load(header_->alive) != 0;
}

void RingBuffer::clear_reader(std::uint32_t i) noexcept {
    if (i < descriptor.readers) {
        shared_store(readers_[i].token, 0);
    }
}

// Only one writer operation may enter for a Publisher. Pick the next physical slot,
// close its old read gate, then wait for announced readers before granting writable bytes.
vinc::Result<Reservation> RingBuffer::reserve(const std::atomic<bool>& closed) {
    if (!writable_ || !alive() || closed.load()) {
        return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Reserve));
    }

    const auto head = shared_load(header_->head);
    const auto generation = shared_load(header_->generation);
    if (head == UINT64_MAX || generation >= maximum_generation) {
        return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Reserve));
    }

    const auto index = static_cast<std::uint32_t>(head % descriptor.slots);
    auto& slot = slots_[index];
    const auto old_token = shared_load(slot.gate);
    if ((old_token & gate_state_mask) == ready) {
        auto expected = old_token;
        if (!shared_compare_exchange(slot.gate, expected, (old_token & ~gate_state_mask) | draining)) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Reserve));
        }

        std::uint64_t wait_started = 0;
        std::uint32_t retry_attempts = 0;
        // An OS-suspended reader can hold this wait indefinitely. Record contention instead
        // of overwriting live bytes; explicit close/retirement still interrupts the wait.
        for (;;) {
            bool reader_holds_slot = false;
            for (std::uint32_t i = 0; i < descriptor.readers; ++i) {
                if (shared_load(readers_[i].token) == old_token) {
                    reader_holds_slot = true;
                    break;
                }
            }

            const bool publisher_closed = !alive() || closed.load();
            if (!reader_holds_slot || publisher_closed) {
                if (wait_started) {
                    const auto elapsed = osal::monotonic_nanoseconds() - wait_started;
                    shared_store(header_->wait_total,
                                 saturated_add(shared_load(header_->wait_total), elapsed));
                    shared_store(header_->wait_max, (std::max)(shared_load(header_->wait_max), elapsed));
                    shared_store(header_->wait_start, 0);
                }

                if (publisher_closed) {
                    return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Reserve));
                }
                break;
            }

            if (!wait_started) {
                wait_started = osal::monotonic_nanoseconds();
                shared_store(header_->wait_start, wait_started);
                shared_store(header_->collisions, saturated_add(shared_load(header_->collisions), 1));
            }

            if (++retry_attempts < config::message_bus_reader_sleep_threshold) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(config::message_bus_reader_retry_interval);
            }
        }
    } else if (old_token != 0) {
        return std::unexpected(vinc::make_error(ErrorCode::OutstandingLoan, ErrorOperation::Reserve));
    }

    shared_store(header_->generation, generation + 1);
    shared_store(slot.gate, ((generation + 1) << gate_state_bits) | writing);
    auto bytes = data_map_.writable_bytes();
    if (!bytes) {
        return std::unexpected(io(bytes.error(), ErrorOperation::Reserve));
    }

    return Reservation{index, head + 1, generation + 1,
                       bytes->subspan(index * slot_stride(descriptor.bytes), descriptor.bytes)};
}

// Publish metadata first, then the ready token, then head. Readers must observe
// both readiness and a sequence no newer than head before treating a slot as published.
vinc::Result<void> RingBuffer::publish(const Reservation& reservation, std::uint64_t bytes, std::uint64_t timestamp,
                                 std::uint64_t count) {
    if (!alive()) {
        return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Publish));
    }

    auto& slot = slots_[reservation.slot];
    if (!writable_ || !bytes || bytes > descriptor.bytes ||
        shared_load(slot.gate) != ((reservation.generation << gate_state_bits) | writing)) {
        return std::unexpected(vinc::make_error(ErrorCode::InvalidLoan, ErrorOperation::Publish));
    }

    shared_store(slot.sequence, reservation.sequence);
    shared_store(slot.timestamp, timestamp);
    shared_store(slot.count, count);
    shared_store(slot.size, bytes);
    shared_store(slot.gate, (reservation.generation << gate_state_bits) | ready);
    // A ready slot above head is still unpublished. This is the publication point.
    shared_store(header_->head, reservation.sequence);
    return {};
}

void RingBuffer::abandon(const Reservation& reservation) noexcept {
    auto expected = (reservation.generation << gate_state_bits) | writing;
    (void)shared_compare_exchange(slots_[reservation.slot].gate, expected, 0);
}

// Find a currently readable sequence within the retained window. This is only a
// metadata candidate: the caller must protect/revalidate it before touching payload bytes.
std::optional<Metadata> RingBuffer::candidate(std::uint64_t cursor, bool latest, std::uint64_t upper) {
    if (!alive()) {
        return {};
    }

    const auto published = shared_load(header_->head);
    const auto head = (std::min)(published, upper);
    if (!head || (!latest && cursor >= head)) {
        return {};
    }

    const auto oldest = published >= descriptor.slots ? published - descriptor.slots + 1 : 1;
    if (!latest && oldest > head) {
        return {};
    }
    auto sequence = latest ? head : (std::max)(oldest, cursor + 1);
    for (;;) {
        const auto index = static_cast<std::uint32_t>((sequence - 1) % descriptor.slots);
        auto& slot = slots_[index];
        const auto gate = shared_load(slot.gate);
        if ((gate & gate_state_mask) == ready) {
            Metadata metadata{shared_load(slot.sequence),
                              shared_load(slot.timestamp),
                              shared_load(slot.count),
                              shared_load(slot.size),
                              gate,
                              index};
            if (metadata.sequence == sequence && shared_load(slot.gate) == gate &&
                metadata.size >= minimum_message_bytes && metadata.size <= descriptor.bytes) {
                return metadata;
            }
        }

        if (latest || sequence == head) {
            return {};
        }
        ++sequence;
    }
}

// Announce first, then recheck the exact generation/state. If the writer won the
// race, discard this candidate and retry without copying any potentially changing bytes.
bool RingBuffer::protect(const Metadata& metadata, std::uint32_t reader_index) {
    if (reader_index >= descriptor.readers) {
        return false;
    }
    // SC announce/revalidate pairs with the writer's SC close/scan. If validation
    // precedes close, scan observes this lease (or its completed release). If the
    // scan preceded announcement, validation sees closed/new generation.
    shared_store(readers_[reader_index].token, metadata.token);
    if (alive() && shared_load(slots_[metadata.slot].gate) == metadata.token) {
        return true;
    }

    clear_reader(reader_index);
    return false;
}

void RingBuffer::unprotect(std::uint32_t reader_index) noexcept {
    clear_reader(reader_index);
}

std::span<const std::byte> RingBuffer::payload(const Metadata& metadata) const {
    return data_map_.bytes().subspan(metadata.slot * slot_stride(descriptor.bytes), metadata.size);
}

PublisherStats RingBuffer::stats(std::uint32_t requested) {
    PublisherStats snapshot;
    snapshot.requested_slots = requested;
    snapshot.effective_slots = descriptor.slots;
    snapshot.max_message_bytes = descriptor.bytes;
    snapshot.shared_bytes = control_bytes(descriptor.slots, descriptor.readers) +
                            descriptor.slots * slot_stride(descriptor.bytes);
    snapshot.published = shared_load(header_->head);
    snapshot.collisions = shared_load(header_->collisions);
    snapshot.total_wait_ns = shared_load(header_->wait_total);
    snapshot.longest_wait_ns = shared_load(header_->wait_max);
    const auto start = shared_load(header_->wait_start);
    if (start) {
        snapshot.current_wait_ns = osal::monotonic_nanoseconds() - start;
    }

    return snapshot;
}

NamedCounter::~NamedCounter() {
    if (creator) {
        unlink(name, resources);
    }
}

namespace {
// Retain each successful mapping in the owner before the next fallible step.
// The owner unmaps/closes it, and only the creator unlinks the name on failure.
vinc::Result<void> map_counter(NamedCounter& counter) {
    auto map = counter.memory.map(sizeof(SharedWord), osal::SharedMemoryAccess::ReadWrite);
    if (!map) {
        return std::unexpected(io(map.error(), ErrorOperation::MapCounter));
    }

    counter.mapping = std::move(*map);
    return {};
}

vinc::Result<void> initialize_counter(NamedCounter& counter) {
    auto bytes = counter.mapping.writable_bytes();
    if (!bytes) {
        return std::unexpected(io(bytes.error(), ErrorOperation::InitializeCounter));
    }
    ::new (bytes->data()) SharedWord{};
    return {};
}
}

vinc::Result<std::shared_ptr<NamedCounter>> NamedCounter::create(std::string name, std::shared_ptr<vinc::detail::ResourceNames> resources) {
    auto counter = std::make_shared<NamedCounter>();
    counter->name = std::move(name);
    counter->resources = std::move(resources);
    auto memory = osal::SharedMemory::create(counter->name, sizeof(SharedWord));
    if (!memory) {
        return std::unexpected(io(memory.error(), ErrorOperation::CreateCounter));
    }

    counter->creator = true;
    counter->memory = std::move(*memory);
    auto mapped = map_counter(*counter);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    auto initialized = initialize_counter(*counter);
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    return counter;
}

vinc::Result<std::shared_ptr<NamedCounter>> NamedCounter::open(std::string name) {
    auto counter = std::make_shared<NamedCounter>();
    counter->name = std::move(name);
    auto memory = osal::SharedMemory::open(counter->name, osal::SharedMemoryAccess::ReadWrite);
    if (!memory) {
        return std::unexpected(io(memory.error(), ErrorOperation::OpenCounter));
    }

    counter->memory = std::move(*memory);
    auto mapped = map_counter(*counter);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    return counter;
}

// Allocate a topic-wide diagnostic count. It does not serialize ring buffer publication
// and is not the FIFO merge key; stop before wrapping and reusing an old count.
vinc::Result<std::uint64_t> NamedCounter::next() {
    auto bytes = mapping.writable_bytes();
    if (!bytes) {
        return std::unexpected(io(bytes.error(), ErrorOperation::NextCount));
    }
    // NamedCounter maps exactly one aligned SharedWord. create() began its lifetime;
    // participants access that same native layout only through OSAL shared atomics.
    auto& word = *reinterpret_cast<SharedWord*>(bytes->data());
    auto value = shared_load(word);
    for (;;) {
        if (value == UINT64_MAX) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::NextCount));
        }

        if (shared_compare_exchange(word, value, value + 1)) {
            return value + 1;
        }
    }
}
}
