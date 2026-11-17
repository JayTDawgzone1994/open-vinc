#pragma once

#include <vinc/message_bus/detail/protocol.hpp>
#include <algorithm>

namespace vinc::message_bus::detail {
// A transfer serial distinguishes a replacement snapshot from an abandoned
// transfer of the same registry revision. A generation belongs to one Session.
struct TopologyPageId {
    std::uint64_t generation;
    std::uint64_t transfer;
    std::uint32_t index;
    bool operator==(const TopologyPageId&) const = default;
};

enum class TopologyMode : std::uint32_t { Delta = 1, Snapshot = 2 };
enum class TopologyReceipt : std::uint32_t { Staged = 1, Applied = 2, Resync = 3 };

// The descriptor is common to every local subscription in this record. Reader
// indices and floors belong to individual relationships, not to the Publisher.
struct TopologyReader {
    std::uint64_t subscription;
    std::uint32_t index;
    std::uint64_t floor;
    bool operator==(const TopologyReader&) const = default;
};

struct SourceGroup {
    Descriptor publisher;
    std::vector<TopologyReader> readers;
};

struct TopologyPage {
    TopologyPageId id;
    std::uint64_t base;
    std::uint64_t target;
    TopologyMode mode;
    bool final;
    // Borrows from the received frame. The caller validates all records
    // before staging them or acknowledging this page.
    std::span<const std::byte> records;
};

struct TopologyAck {
    TopologyPageId id;
    TopologyReceipt receipt;
};

inline constexpr std::size_t topology_id_bytes = 2 * sizeof(std::uint64_t) + sizeof(std::uint32_t);
inline constexpr std::size_t topology_header_bytes =
    topology_id_bytes + 2 * sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t);
inline constexpr std::size_t topology_page_capacity = control::max_payload - topology_header_bytes;
inline constexpr std::size_t topology_reader_bytes = 2 * sizeof(std::uint64_t) + sizeof(std::uint32_t);

inline bool valid_shared_name(std::string_view name) {
    return !name.empty() && name.size() <= 24 && std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

inline bool valid_source_descriptor(const Descriptor& d) {
    return d.id && d.owner && d.rank && d.schema && d.slots >= minimum_ring_buffer_slots &&
           d.slots <= maximum_ring_buffer_slots && d.readers && d.readers <= maximum_ring_buffer_readers &&
           d.bytes >= minimum_message_bytes && d.bytes <= SIZE_MAX - payload_alignment_mask &&
           RingBuffer::slot_stride(d.bytes) <= SIZE_MAX / d.slots && valid_shared_name(d.control) &&
           valid_shared_name(d.data) && valid_shared_name(d.counter) && !d.publisher.empty() &&
           d.publisher.size() <= maximum_name_bytes && !d.publisher.contains('\0');
}

inline Result<control::Bytes> encode_source_group(const Descriptor& publisher, std::span<const TopologyReader> readers) {
    if (!valid_source_descriptor(publisher) || readers.empty() || readers.size() > publisher.readers) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EncodeTopology});
    }
    control::Bytes bytes;
    auto encoded = encode_descriptor(bytes, publisher);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    control::put_u32(bytes, static_cast<std::uint32_t>(readers.size()));
    if (bytes.size() + topology_reader_bytes * readers.size() > topology_page_capacity) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::EncodeTopology});
    }
    std::uint64_t previous = 0;
    for (const auto& reader : readers) {
        if (reader.subscription <= previous || reader.index >= publisher.readers) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EncodeTopology});
        }
        control::put_u64(bytes, reader.subscription);
        control::put_u32(bytes, reader.index);
        control::put_u64(bytes, reader.floor);
        previous = reader.subscription;
    }
    return bytes;
}

inline Result<SourceGroup> decode_source_group(std::span<const std::byte> bytes) {
    if (bytes.size() > topology_page_capacity) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    control::Reader reader(bytes);
    SourceGroup group{decode_descriptor(reader), {}};
    const auto count = reader.u32();
    if (!valid_source_descriptor(group.publisher) || !count || count > group.publisher.readers ||
        count > bytes.size() / topology_reader_bytes) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    group.readers.reserve(count);
    std::uint64_t previous = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        TopologyReader entry{reader.u64(), reader.u32(), reader.u64()};
        if (entry.subscription <= previous || entry.index >= group.publisher.readers) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }
        previous = entry.subscription;
        group.readers.push_back(entry);
    }
    if (!reader.done()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    return group;
}

inline bool valid_page_id(TopologyPageId id) {
    return id.generation != 0 && id.transfer != 0;
}

inline bool valid_page(const TopologyPage& page) {
    if (!valid_page_id(page.id) || !page.target || page.records.size() > topology_page_capacity) {
        return false;
    }
    if (page.records.empty() && !(page.mode == TopologyMode::Snapshot && page.final && page.id.index == 0)) {
        return false;
    }
    if (!page.final && page.id.index == UINT32_MAX) {
        return false;
    }
    if (page.mode == TopologyMode::Snapshot) {
        return page.base == 0;
    }
    return page.mode == TopologyMode::Delta && page.target > page.base;
}

inline void encode_page_id(control::Bytes& bytes, TopologyPageId id) {
    control::put_u64(bytes, id.generation);
    control::put_u64(bytes, id.transfer);
    control::put_u32(bytes, id.index);
}

inline TopologyPageId decode_page_id(control::Reader& reader) {
    return {reader.u64(), reader.u64(), reader.u32()};
}

inline Result<control::Bytes> encode_topology_page(const TopologyPage& page) {
    if (!valid_page(page)) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EncodeTopology});
    }
    control::Bytes bytes;
    bytes.reserve(topology_header_bytes + page.records.size());
    encode_page_id(bytes, page.id);
    control::put_u64(bytes, page.base);
    control::put_u64(bytes, page.target);
    control::put_u32(bytes, static_cast<std::uint32_t>(page.mode));
    control::put_u32(bytes, page.final ? 1u : 0u);
    bytes.insert(bytes.end(), page.records.begin(), page.records.end());
    return bytes;
}

inline Result<TopologyPage> decode_topology_page(std::span<const std::byte> bytes) {
    if (bytes.size() < topology_header_bytes || bytes.size() > control::max_payload) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    control::Reader reader(bytes.first(topology_header_bytes));
    const auto id = decode_page_id(reader);
    const auto base = reader.u64();
    const auto target = reader.u64();
    const auto mode = static_cast<TopologyMode>(reader.u32());
    const auto final = reader.u32();
    TopologyPage page{id, base, target, mode, final == 1, bytes.subspan(topology_header_bytes)};
    if (!reader.done() || final > 1 || !valid_page(page)) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    return page;
}

inline bool valid_receipt(TopologyReceipt receipt) {
    return receipt == TopologyReceipt::Staged || receipt == TopologyReceipt::Applied ||
           receipt == TopologyReceipt::Resync;
}

inline Result<control::Bytes> encode_topology_ack(TopologyAck ack) {
    if (!valid_page_id(ack.id) || !valid_receipt(ack.receipt)) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EncodeTopology});
    }
    control::Bytes bytes;
    encode_page_id(bytes, ack.id);
    control::put_u32(bytes, static_cast<std::uint32_t>(ack.receipt));
    return bytes;
}

inline Result<TopologyAck> decode_topology_ack(std::span<const std::byte> bytes) {
    control::Reader reader(bytes);
    const auto id = decode_page_id(reader);
    const auto receipt = static_cast<TopologyReceipt>(reader.u32());
    if (!reader.done() || !valid_page_id(id) || !valid_receipt(receipt)) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    return TopologyAck{id, receipt};
}

// Each page carries a sequence of length-delimited records. The SnapshotCut
// is the last allocated endpoint ID when the worker froze this transfer.
inline constexpr std::uint32_t snapshot_cut = 1;
inline constexpr std::size_t topology_record_header = 2 * sizeof(std::uint32_t);
inline constexpr std::size_t topology_group_readers = 128;

inline void append_topology_record(control::Bytes& page, std::uint32_t kind, const control::Bytes& body) {
    control::put_u32(page, kind);
    control::put_u32(page, static_cast<std::uint32_t>(body.size()));
    page.insert(page.end(), body.begin(), body.end());
}

template<class Visitor>
Result<void> visit_topology_records(std::span<const std::byte> bytes, Visitor&& visit) {
    const Error malformed{ErrorCode::Protocol, ErrorOperation::Receive};
    while (!bytes.empty()) {
        if (bytes.size() < topology_record_header) {
            return std::unexpected(malformed);
        }
        control::Reader header(bytes.first(topology_record_header));
        const auto kind = header.u32();
        const auto size = header.u32();
        bytes = bytes.subspan(topology_record_header);
        if (!size || size > bytes.size()) {
            return std::unexpected(malformed);
        }
        auto result = visit(kind, bytes.first(size));
        if (!result) {
            return result;
        }
        bytes = bytes.subspan(size);
    }
    return {};
}

enum class PageAction { Stage, Commit, RepeatReceipt, RequestSnapshot, IgnoreOld };

enum class AckAction { IgnoreOld, NextPage, Finished, RestartSnapshot };

// The Bus worker owns this cursor. A full transport queue leaves the page
// pending admission; only a successful send creates an outstanding receipt.
class TopologySendCursor {
    std::uint64_t generation_;
    std::uint64_t transfer_ = 0;
    std::uint32_t index_ = 0;
    std::uint64_t applied_ = 0;
    std::uint64_t target_ = 0;
    bool active_ = false;
    bool awaiting_ = false;
    bool final_ = false;

public:
    explicit TopologySendCursor(std::uint64_t generation) : generation_(generation) {}

    Result<void> start(std::uint64_t target) {
        if (active_ || !generation_ || !target || target < applied_) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EncodeTopology});
        }
        if (transfer_ == UINT64_MAX) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::EncodeTopology});
        }
        ++transfer_;
        index_ = 0;
        target_ = target;
        active_ = true;
        return {};
    }

    // A staged receipt releases the only in-flight page. If the undo history
    // overflowed, the worker may now replace its uncommitted snapshot.
    void supersede_staged() noexcept {
        active_ = false;
        awaiting_ = false;
    }

    bool ready_to_send() const noexcept {
        return active_ && !awaiting_;
    }

    TopologyPageId id() const noexcept {
        return {generation_, transfer_, index_};
    }

    std::uint64_t applied() const noexcept {
        return applied_;
    }

    void sent(bool final) noexcept {
        awaiting_ = true;
        final_ = final;
    }

    Result<AckAction> acknowledge(TopologyAck ack) {
        if (ack.id.generation != generation_ || ack.id.transfer < transfer_ ||
            (ack.id.transfer == transfer_ && (ack.id.index < index_ || !active_))) {
            return AckAction::IgnoreOld;
        }
        if (!awaiting_ || ack.id != id()) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }
        if (ack.receipt == TopologyReceipt::Resync) {
            active_ = false;
            awaiting_ = false;
            return AckAction::RestartSnapshot;
        }
        if (final_ && ack.receipt == TopologyReceipt::Applied) {
            applied_ = target_;
            active_ = false;
            awaiting_ = false;
            return AckAction::Finished;
        }
        if (!final_ && ack.receipt == TopologyReceipt::Staged && index_ != UINT32_MAX) {
            ++index_;
            awaiting_ = false;
            return AckAction::NextPage;
        }
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
};

// Owned by the single Host dispatcher. Classification never acknowledges or
// advances a page. Call accept() only after its records were staged or committed.
class TopologyReceiveCursor {
    std::uint64_t generation_;
    std::uint64_t applied_ = 0;
    std::optional<TopologyPageId> last_;
    TopologyMode mode_ = TopologyMode::Delta;
    std::uint64_t base_ = 0;
    std::uint64_t target_ = 0;
    bool final_ = false;

public:
    explicit TopologyReceiveCursor(std::uint64_t generation) : generation_(generation) {}

    PageAction classify(const TopologyPage& page) const noexcept {
        if (page.id.generation != generation_ || (last_ && page.id.transfer < last_->transfer)) {
            return PageAction::IgnoreOld;
        }
        if (last_ && page.id.transfer == last_->transfer) {
            if (page.id.index < last_->index) {
                return PageAction::IgnoreOld;
            }
            if (page.mode != mode_ || page.base != base_ || page.target != target_) {
                return PageAction::RequestSnapshot;
            }
            if (page.id.index == last_->index) {
                return page.final == final_ ? PageAction::RepeatReceipt : PageAction::RequestSnapshot;
            }
            if (final_ || page.id.index != last_->index + 1) {
                return PageAction::RequestSnapshot;
            }
        } else {
            if (page.id.index != 0 || page.target < applied_) {
                return PageAction::RequestSnapshot;
            }
            if (page.mode == TopologyMode::Delta && (page.base != applied_ || (last_ && !final_))) {
                return PageAction::RequestSnapshot;
            }
        }
        return page.final ? PageAction::Commit : PageAction::Stage;
    }

    void accept(const TopologyPage& page) noexcept {
        last_ = page.id;
        mode_ = page.mode;
        base_ = page.base;
        target_ = page.target;
        final_ = page.final;
        if (final_) {
            applied_ = target_;
        }
    }

    std::uint64_t applied() const noexcept {
        return applied_;
    }
};
}
