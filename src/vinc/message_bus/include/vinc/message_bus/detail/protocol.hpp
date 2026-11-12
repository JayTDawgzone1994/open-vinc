#pragma once

// Message Bus control-plane codec: exchange ring buffer descriptors, reader identities and topology.
// These frames carry shared-memory names and sizes, never the message payload itself.

#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <vinc/message_bus/detail/runtime.hpp>
#include <vinc/message_bus/detail/error_protocol.hpp>
#include <string_view>

namespace vinc::message_bus::detail {
// Bounded control descriptors; these are protocol/admission limits, not queue defaults.
inline constexpr std::uint32_t maximum_attached_publishers = 32;
inline constexpr std::size_t maximum_name_bytes = 256;
inline constexpr std::uint32_t topics_per_page = 32;
inline constexpr std::size_t response_status_bytes = sizeof(std::uint32_t);
inline constexpr std::uint64_t minimum_bus_manager_shared_bytes = 1024;

// Topic separators describe user naming, never Module ownership. Keep this
// check separate from existing length/encoding validation and its diagnostics.
inline bool valid_topic_separators(std::string_view topic) noexcept {
    return topic.find('.') == std::string_view::npos;
}

enum Operation : std::uint16_t {
    // High byte is the Bus protocol version. v1 used 1..6; disjoint identities
    // make old peers reject v5 before interpreting acknowledged release batches.
    Advertise = 0x0501,
    Subscribe = 0x0502,
    ClosePublisher = 0x0503,
    Unsubscribe = 0x0504,
    Attach = 0x0505,
    Detach = 0x0506,
    ListTopics = 0x0507,
    InspectTopics = 0x0508,
    ListTopicConnections = 0x0509,
    EnableNotifications = 0x050a,
    NotificationRoutesChanged = 0x050b,
    ReleaseBatch = 0x050c
};

// Bounds the cleanup work admitted by one control frame.
inline constexpr std::uint32_t maximum_releases_per_batch = 16;
struct EndpointRelease {
    std::uint16_t operation;
    std::uint64_t id;
};

inline constexpr std::uint32_t publisher_slots_present = 1;
inline constexpr std::uint32_t publisher_bytes_present = 2;
inline constexpr std::uint32_t publisher_options_mask = publisher_slots_present | publisher_bytes_present;

inline bool supported_operation(std::uint16_t operation) noexcept {
    return operation >= Advertise && operation <= ReleaseBatch;
}

inline vinc::Result<void> encode_descriptor(control::Bytes& bytes, const Descriptor& descriptor) {
    control::put_u64(bytes, descriptor.id);
    control::put_u64(bytes, descriptor.owner);
    control::put_u64(bytes, descriptor.schema);
    control::put_u64(bytes, descriptor.rank);
    control::put_u32(bytes, descriptor.slots);
    control::put_u32(bytes, descriptor.readers);
    control::put_u64(bytes, descriptor.bytes);
    for (const auto* text :
         {&descriptor.control, &descriptor.data, &descriptor.counter, &descriptor.publisher}) {
        auto encoded = control::put_string(bytes, *text);
        if (!encoded) {
            return std::unexpected(
                wrap_error(ErrorCode::CapacityExceeded, ErrorOperation::EncodeDescriptor, encoded.error()));
        }
    }
    return {};
}

inline Descriptor decode_descriptor(control::Reader& reader) {
    Descriptor descriptor;
    descriptor.id = reader.u64();
    descriptor.owner = reader.u64();
    descriptor.schema = reader.u64();
    descriptor.rank = reader.u64();
    descriptor.slots = reader.u32();
    descriptor.readers = reader.u32();
    descriptor.bytes = reader.u64();
    descriptor.control = reader.string();
    descriptor.data = reader.string();
    descriptor.counter = reader.string();
    descriptor.publisher = reader.string();
    return descriptor;
}

inline vinc::Result<void> encode_attachment(control::Bytes& bytes, const Attachment& attachment) {
    auto encoded = encode_descriptor(bytes, attachment.descriptor);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    control::put_u32(bytes, attachment.reader);
    control::put_u64(bytes, attachment.floor);
    return {};
}

inline Attachment decode_attachment(control::Reader& reader) {
    Attachment attachment;
    attachment.descriptor = decode_descriptor(reader);
    attachment.reader = reader.u32();
    attachment.floor = reader.u64();
    return attachment;
}
}
