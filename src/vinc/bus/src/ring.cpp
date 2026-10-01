#include "ring.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <thread>

namespace vinc::bus::detail {
namespace {
constexpr std::uint64_t magic = 0x31474e495242564fULL;
constexpr std::uint64_t ready = 1, draining = 2, writing = 3;
Error io(const osal::OsError& e) {
    return error(ErrorCode::Io, "shared memory operation=" + std::to_string(static_cast<unsigned>(e.operation)) +
        " code=" + std::to_string(static_cast<unsigned>(e.code)) + " native=" + std::to_string(e.native.value()) + ": " + e.native.message());
}
void unlink(const std::string& name) noexcept { if (!name.empty()) (void)osal::SharedMemory::unlink_name(name); }
std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
}
Error error(ErrorCode c, std::string text) { return {c, std::move(text)}; }
Error storage_error(message::Error e) {
    switch (e.code) {
    case message::ErrorCode::CapacityExceeded: return error(ErrorCode::CapacityExceeded, "message exceeds publisher byte budget");
    case message::ErrorCode::ResourceExhausted: return error(ErrorCode::ResourceExhausted, "message allocation failed");
    case message::ErrorCode::TypeMismatch: return error(ErrorCode::TypeMismatch, "message schema mismatch");
    case message::ErrorCode::Closed: return error(ErrorCode::Closed, "message loan is closed");
    default: return error(ErrorCode::InvalidData, "invalid message storage");
    }
}
std::string shared_name(char suffix) {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string name = "vb";
    for (int i = 0; i < 2; ++i) { const auto n = random(); for (int b = 28; b >= 0; b -= 4) name += hex[(n >> b) & 15]; }
    name += suffix; return name;
}
std::uint64_t Ring::control_bytes(std::uint32_t slots, std::uint32_t readers) {
    return sizeof(RingHeader) + std::uint64_t(slots) * sizeof(Slot) + std::uint64_t(readers) * sizeof(ReaderSlot);
}
Ring::~Ring() {
    if (creator_) { retire(); if (control_) unlink(descriptor.control); if (data_) unlink(descriptor.data); }
}
Result<std::shared_ptr<Ring>> Ring::create(Descriptor d) {
    if (d.slots < 2 || d.slots > 65536 || !d.readers || d.readers > 4096 || d.bytes < 64 || d.bytes > SIZE_MAX - 7 || slot_stride(d.bytes) > SIZE_MAX / d.slots)
        return std::unexpected(error(ErrorCode::InvalidArgument, "invalid ring dimensions"));
    auto r = std::make_shared<Ring>(); r->descriptor = std::move(d); r->creator_ = true; r->writable_ = true;
    auto c = osal::SharedMemory::create(r->descriptor.control, control_bytes(r->descriptor.slots, r->descriptor.readers));
    if (!c) { r->creator_ = false; return std::unexpected(io(c.error())); }
    r->control_ = std::move(*c);
    auto m = r->control_.map(control_bytes(r->descriptor.slots, r->descriptor.readers), osal::SharedMemoryAccess::ReadWrite);
    if (!m) return std::unexpected(io(m.error())); r->control_map_ = std::move(*m);
    auto writable = r->control_map_.writable_bytes(); if (!writable) return std::unexpected(io(writable.error()));
    r->header_ = ::new(writable->data()) RingHeader{};
    r->header_->magic = magic; r->header_->id = r->descriptor.id; r->header_->schema = r->descriptor.schema;
    r->header_->slots = r->descriptor.slots; r->header_->bytes = r->descriptor.bytes; r->header_->readers = r->descriptor.readers;
    r->slots_ = reinterpret_cast<Slot*>(writable->data() + sizeof(RingHeader));
    for (std::uint32_t i = 0; i < r->descriptor.slots; ++i) ::new(r->slots_ + i) Slot{};
    r->readers_ = reinterpret_cast<ReaderSlot*>(writable->data() + sizeof(RingHeader) + r->descriptor.slots * sizeof(Slot));
    for (std::uint32_t i = 0; i < r->descriptor.readers; ++i) ::new(r->readers_ + i) ReaderSlot{};
    auto data = osal::SharedMemory::create(r->descriptor.data, r->descriptor.slots * slot_stride(r->descriptor.bytes));
    if (!data) { r->descriptor.data.clear(); return std::unexpected(io(data.error())); } r->data_ = std::move(*data);
    auto dm = r->data_.map(r->descriptor.slots * slot_stride(r->descriptor.bytes), osal::SharedMemoryAccess::ReadWrite);
    if (!dm) return std::unexpected(io(dm.error())); r->data_map_ = std::move(*dm);
    shared_store(r->header_->alive, 1); return r;
}
Result<std::shared_ptr<Ring>> Ring::open(Descriptor d, bool writer) {
    if (d.slots < 2 || !d.readers || d.slots > 65536 || d.readers > 4096 || d.bytes < 64 || d.bytes > SIZE_MAX - 7 || slot_stride(d.bytes) > SIZE_MAX / d.slots)
        return std::unexpected(error(ErrorCode::Protocol, "invalid ring descriptor"));
    auto r = std::make_shared<Ring>(); r->descriptor = std::move(d); r->writable_ = writer;
    auto c = osal::SharedMemory::open(r->descriptor.control, osal::SharedMemoryAccess::ReadWrite);
    if (!c) return std::unexpected(io(c.error())); r->control_ = std::move(*c);
    auto cm = r->control_.map(control_bytes(r->descriptor.slots, r->descriptor.readers), osal::SharedMemoryAccess::ReadWrite);
    if (!cm) return std::unexpected(io(cm.error())); r->control_map_ = std::move(*cm);
    auto writable = r->control_map_.writable_bytes(); if (!writable) return std::unexpected(io(writable.error()));
    r->header_ = reinterpret_cast<RingHeader*>(writable->data());
    const auto& h = *r->header_;
    if (h.magic != magic || h.version != 1 || h.id != r->descriptor.id || h.schema != r->descriptor.schema ||
        h.slots != r->descriptor.slots || h.bytes != r->descriptor.bytes || h.readers != r->descriptor.readers)
        return std::unexpected(error(ErrorCode::Protocol, "shared ring header mismatch"));
    r->slots_ = reinterpret_cast<Slot*>(writable->data() + sizeof(RingHeader));
    r->readers_ = reinterpret_cast<ReaderSlot*>(writable->data() + sizeof(RingHeader) + h.slots * sizeof(Slot));
    const auto access = writer ? osal::SharedMemoryAccess::ReadWrite : osal::SharedMemoryAccess::ReadOnly;
    auto data = osal::SharedMemory::open(r->descriptor.data, access);
    if (!data) return std::unexpected(io(data.error())); r->data_ = std::move(*data);
    auto dm = r->data_.map(h.slots * slot_stride(h.bytes), access);
    if (!dm) return std::unexpected(io(dm.error())); r->data_map_ = std::move(*dm);
    if (!r->alive()) return std::unexpected(error(ErrorCode::Closed, "publisher retired"));
    return r;
}
void Ring::retire() noexcept { if (header_) shared_store(header_->alive, 0); }
bool Ring::alive() const noexcept { return header_ && shared_load(header_->alive) != 0; }
void Ring::clear_reader(std::uint32_t i) noexcept { if (i < descriptor.readers) shared_store(readers_[i].token, 0); }
Result<Reservation> Ring::reserve(const std::atomic<bool>& closed) {
    if (!writable_ || !alive() || closed.load()) return std::unexpected(error(ErrorCode::Closed, "publisher closed"));
    const auto head = shared_load(header_->head), generation = shared_load(header_->generation);
    if (head == UINT64_MAX || generation >= (UINT64_MAX >> 2) - 1)
        return std::unexpected(error(ErrorCode::ResourceExhausted, "ring identifiers exhausted"));
    const auto index = static_cast<std::uint32_t>(head % descriptor.slots);
    auto& s = slots_[index];
    const auto old = shared_load(s.gate);
    if ((old & 3) == ready) {
        auto expected = old;
        if (!shared_compare_exchange(s.gate, expected, (old & ~3ULL) | draining))
            return std::unexpected(error(ErrorCode::Protocol, "concurrent writer on publisher ring"));
        std::uint64_t started = 0;
        std::uint32_t spins = 0;
        for (;;) {
            bool held = false;
            for (std::uint32_t i = 0; i < descriptor.readers; ++i) if (shared_load(readers_[i].token) == old) { held = true; break; }
            const bool ended = !alive() || closed.load();
            if (!held || ended) {
                if (started) {
                    const auto elapsed = osal::monotonic_nanoseconds() - started;
                    shared_store(header_->wait_total, saturated_add(shared_load(header_->wait_total), elapsed));
                    shared_store(header_->wait_max, (std::max)(shared_load(header_->wait_max), elapsed));
                    shared_store(header_->wait_start, 0);
                }
                if (ended) return std::unexpected(error(ErrorCode::Closed, "publisher closed while waiting"));
                break;
            }
            if (!started) {
                started = osal::monotonic_nanoseconds(); shared_store(header_->wait_start, started);
                shared_store(header_->collisions, saturated_add(shared_load(header_->collisions), 1));
            }
            if (++spins < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else if (old != 0) return std::unexpected(error(ErrorCode::OutstandingLoan, "slot is already borrowed"));
    shared_store(header_->generation, generation + 1);
    shared_store(s.gate, ((generation + 1) << 2) | writing);
    auto bytes = data_map_.writable_bytes(); if (!bytes) return std::unexpected(io(bytes.error()));
    return Reservation{index, head + 1, generation + 1, bytes->subspan(index * slot_stride(descriptor.bytes), descriptor.bytes)};
}
Result<void> Ring::publish(const Reservation& r, std::uint64_t bytes, std::uint64_t timestamp, std::uint64_t count) {
    if (!alive()) return std::unexpected(error(ErrorCode::Closed, "publisher retired"));
    auto& s = slots_[r.slot];
    if (!writable_ || !bytes || bytes > descriptor.bytes || shared_load(s.gate) != ((r.generation << 2) | writing))
        return std::unexpected(error(ErrorCode::InvalidLoan, "invalid write reservation"));
    shared_store(s.sequence, r.sequence); shared_store(s.timestamp, timestamp); shared_store(s.count, count); shared_store(s.size, bytes);
    shared_store(s.gate, (r.generation << 2) | ready);
    // A ready slot above head is still unpublished. This is the publication point.
    shared_store(header_->head, r.sequence);
    return {};
}
void Ring::abandon(const Reservation& r) noexcept {
    auto expected = (r.generation << 2) | writing;
    (void)shared_compare_exchange(slots_[r.slot].gate, expected, 0);
}
std::optional<Metadata> Ring::candidate(std::uint64_t cursor, bool latest) {
    if (!alive()) return {};
    const auto head = shared_load(header_->head);
    if (!head || (!latest && cursor >= head)) return {};
    const auto oldest = head >= descriptor.slots ? head - descriptor.slots + 1 : 1;
    auto sequence = latest ? head : (std::max)(oldest, cursor + 1);
    for (;;) {
        const auto index = static_cast<std::uint32_t>((sequence - 1) % descriptor.slots);
        auto& s = slots_[index]; const auto gate = shared_load(s.gate);
        if ((gate & 3) == ready) {
            Metadata m{shared_load(s.sequence), shared_load(s.timestamp), shared_load(s.count), shared_load(s.size), gate, index};
            if (m.sequence == sequence && shared_load(s.gate) == gate && m.size >= 64 && m.size <= descriptor.bytes) return m;
        }
        if (latest || sequence == head) return {};
        ++sequence;
    }
}
bool Ring::protect(const Metadata& m, std::uint32_t reader_index) {
    if (reader_index >= descriptor.readers) return false;
    // SC announce/revalidate pairs with the writer's SC close/scan. If validation
    // precedes close, scan observes this lease (or its completed release). If the
    // scan preceded announcement, validation sees closed/new generation.
    shared_store(readers_[reader_index].token, m.token);
    if (alive() && shared_load(slots_[m.slot].gate) == m.token) return true;
    clear_reader(reader_index); return false;
}
void Ring::unprotect(std::uint32_t i) noexcept { clear_reader(i); }
std::span<const std::byte> Ring::payload(const Metadata& m) const { return data_map_.bytes().subspan(m.slot * slot_stride(descriptor.bytes), m.size); }
PublisherStats Ring::stats(std::uint32_t requested) {
    PublisherStats s;
    s.requested_slots = requested; s.effective_slots = descriptor.slots; s.max_message_bytes = descriptor.bytes;
    s.shared_bytes = control_bytes(descriptor.slots, descriptor.readers) + descriptor.slots * slot_stride(descriptor.bytes);
    s.published = shared_load(header_->head); s.collisions = shared_load(header_->collisions);
    s.total_wait_ns = shared_load(header_->wait_total); s.longest_wait_ns = shared_load(header_->wait_max);
    const auto start = shared_load(header_->wait_start); if (start) s.current_wait_ns = osal::monotonic_nanoseconds() - start;
    return s;
}
NamedCounter::~NamedCounter() { if (creator) unlink(name); }
Result<std::shared_ptr<NamedCounter>> NamedCounter::create(std::string name) {
    auto r = std::make_shared<NamedCounter>(); r->name = std::move(name);
    auto m = osal::SharedMemory::create(r->name, sizeof(SharedWord)); if (!m) return std::unexpected(io(m.error()));
    r->creator = true; r->memory = std::move(*m);
    auto map = r->memory.map(sizeof(SharedWord), osal::SharedMemoryAccess::ReadWrite); if (!map) return std::unexpected(io(map.error()));
    r->mapping = std::move(*map); auto bytes = r->mapping.writable_bytes();
    if (!bytes) return std::unexpected(io(bytes.error())); ::new(bytes->data()) SharedWord{};
    return r;
}
Result<std::shared_ptr<NamedCounter>> NamedCounter::open(std::string name) {
    auto r = std::make_shared<NamedCounter>(); r->name = std::move(name);
    auto m = osal::SharedMemory::open(r->name, osal::SharedMemoryAccess::ReadWrite); if (!m) return std::unexpected(io(m.error()));
    r->memory = std::move(*m); auto map = r->memory.map(sizeof(SharedWord), osal::SharedMemoryAccess::ReadWrite);
    if (!map) return std::unexpected(io(map.error())); r->mapping = std::move(*map); return r;
}
Result<std::uint64_t> NamedCounter::next() {
    auto bytes = mapping.writable_bytes(); if (!bytes) return std::unexpected(io(bytes.error()));
    auto& word = *reinterpret_cast<SharedWord*>(bytes->data()); auto value = shared_load(word);
    for (;;) {
        if (value == UINT64_MAX) return std::unexpected(error(ErrorCode::ResourceExhausted, "topic count exhausted"));
        if (shared_compare_exchange(word, value, value + 1)) return value + 1;
    }
}
}
