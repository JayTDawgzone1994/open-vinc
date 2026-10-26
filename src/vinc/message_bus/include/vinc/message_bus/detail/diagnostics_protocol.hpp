#pragma once

#include <vinc/message_bus/detail/protocol.hpp>

namespace vinc::message_bus::detail {
// One topic with 32 bounded publisher names fits the existing 16 KiB frame.
inline constexpr std::uint32_t diagnostics_per_page = 1;
inline constexpr std::uint32_t diagnostics_version = 1;

inline Result<control::Bytes> encode_diagnostics(const TopicDiagnosticsPage& page) {
    control::Bytes bytes;
    control::put_u32(bytes, diagnostics_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(page.topics.size()));
    for (const auto& topic : page.topics) {
        auto name = control::put_string(bytes, topic.name);
        if (!name) {
            return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::InspectTopics, name.error()));
        }

        control::put_u64(bytes, topic.schema);
        control::put_u32(bytes, topic.subscribers);
        control::put_u64(bytes, topic.lost_messages);
        control::put_u64(bytes, topic.overwrite_events);
        control::put_u64(bytes, topic.oversized_messages);
        control::put_u32(bytes, static_cast<std::uint32_t>(topic.publishers.size()));
        for (const auto& publisher : topic.publishers) {
            control::put_u64(bytes, publisher.id);
            control::put_u64(bytes, publisher.owner);
            auto label = control::put_string(bytes, publisher.name);
            if (!label) {
                return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::InspectTopics, label.error()));
            }

            const auto& stats = publisher.stats;
            control::put_u32(bytes, stats.requested_slots);
            control::put_u32(bytes, stats.effective_slots);
            control::put_u64(bytes, stats.max_message_bytes);
            control::put_u64(bytes, stats.shared_bytes);
            control::put_u64(bytes, stats.published);
            control::put_u64(bytes, stats.collisions);
            control::put_u64(bytes, stats.total_wait_ns);
            control::put_u64(bytes, stats.longest_wait_ns);
            control::put_u64(bytes, stats.current_wait_ns);
        }
    }

    auto next = control::put_string(bytes, page.next);
    if (!next) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::InspectTopics, next.error()));
    }

    return bytes;
}

inline Result<TopicDiagnosticsPage> decode_diagnostics(std::span<const std::byte> bytes,
                                                       const std::string& after) {
    const auto invalid = [] {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::InspectTopics});
    };
    control::Reader reader(bytes);
    const auto version = reader.u32();
    const auto count = reader.u32();
    if (version != diagnostics_version || count > diagnostics_per_page) {
        return invalid();
    }

    TopicDiagnosticsPage result;
    auto previous = after;
    for (std::uint32_t index = 0; index < count; ++index) {
        TopicDiagnostics topic;
        topic.name = reader.string();
        topic.schema = reader.u64();
        topic.subscribers = reader.u32();
        topic.lost_messages = reader.u64();
        topic.overwrite_events = reader.u64();
        topic.oversized_messages = reader.u64();
        const auto publishers = reader.u32();
        if (topic.name <= previous || topic.name.size() > maximum_name_bytes ||
            topic.name.find('\0') != std::string::npos || !valid_topic_separators(topic.name) || !topic.schema ||
            topic.subscribers > maximum_ring_buffer_readers || publishers > maximum_attached_publishers) {
            return invalid();
        }

        previous = topic.name;
        std::uint64_t previous_id = 0;
        for (std::uint32_t item = 0; item < publishers; ++item) {
            PublisherDiagnostics publisher;
            publisher.id = reader.u64();
            publisher.owner = reader.u64();
            publisher.name = reader.string();
            auto& stats = publisher.stats;
            stats.requested_slots = reader.u32();
            stats.effective_slots = reader.u32();
            stats.max_message_bytes = reader.u64();
            stats.shared_bytes = reader.u64();
            stats.published = reader.u64();
            stats.collisions = reader.u64();
            stats.total_wait_ns = reader.u64();
            stats.longest_wait_ns = reader.u64();
            stats.current_wait_ns = reader.u64();
            if (publisher.id <= previous_id || !publisher.owner || publisher.name.empty() ||
                publisher.name.size() > maximum_name_bytes || publisher.name.find('\0') != std::string::npos ||
                stats.effective_slots < minimum_ring_buffer_slots ||
                stats.effective_slots > maximum_ring_buffer_slots || !stats.requested_slots ||
                stats.requested_slots > maximum_ring_buffer_slots || stats.max_message_bytes < minimum_message_bytes) {
                return invalid();
            }

            previous_id = publisher.id;
            topic.publishers.push_back(std::move(publisher));
        }

        result.topics.push_back(std::move(topic));
    }

    result.next = reader.string();
    if (!reader.done() || (!result.next.empty() && (count == 0 || result.next != previous))) {
        return invalid();
    }

    return result;
}
}
