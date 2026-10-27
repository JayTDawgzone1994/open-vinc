#pragma once

#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/owner_id.hpp>
#include <tuple>

namespace vinc::message_bus::detail {
inline constexpr std::uint32_t connections_version = 1;
// Repeated bounded names plus metadata and a cursor fit the 16 KiB envelope.
inline constexpr std::uint32_t connections_per_page = 32;

inline bool valid_cursor(const ConnectionCursor& cursor) {
    if (cursor.topic.empty()) {
        return cursor.id == 0 && cursor.role == ConnectionRole::Publisher;
    }

    return cursor.topic.size() <= maximum_name_bytes && cursor.topic.find('\0') == std::string::npos &&
        valid_topic_separators(cursor.topic) && cursor.id != 0 &&
        (cursor.role == ConnectionRole::Publisher || cursor.role == ConnectionRole::Subscriber);
}

inline bool cursor_before(const ConnectionCursor& left, const ConnectionCursor& right) {
    return std::tie(left.topic, left.role, left.id) < std::tie(right.topic, right.role, right.id);
}

inline Result<void> encode_cursor(control::Bytes& bytes, const ConnectionCursor& cursor) {
    auto encoded = control::put_string(bytes, cursor.topic);
    if (!encoded) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::ListTopicConnections, encoded.error()));
    }

    control::put_u32(bytes, static_cast<std::uint32_t>(cursor.role));
    control::put_u64(bytes, cursor.id);
    return {};
}

inline ConnectionCursor read_cursor(control::Reader& reader) {
    ConnectionCursor cursor;
    cursor.topic = reader.string();
    cursor.role = static_cast<ConnectionRole>(reader.u32());
    cursor.id = reader.u64();
    return cursor;
}

inline Result<control::Bytes> encode_connections(const TopicConnectionsPage& page) {
    control::Bytes bytes;
    control::put_u32(bytes, connections_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(page.connections.size()));
    for (const auto& connection : page.connections) {
        auto encoded = encode_cursor(bytes, connection.key);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }

        control::put_u64(bytes, connection.schema);
        control::put_u64(bytes, connection.mid);
    }

    auto encoded = encode_cursor(bytes, page.next);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }

    return bytes;
}

inline Result<TopicConnectionsPage> decode_connections(std::span<const std::byte> bytes,
                                                       const ConnectionCursor& after) {
    const auto invalid = [] {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopicConnections});
    };
    control::Reader reader(bytes);
    const auto version = reader.u32();
    const auto count = reader.u32();
    if (version != connections_version || count > connections_per_page) {
        return invalid();
    }

    TopicConnectionsPage page;
    auto previous = after;
    for (std::uint32_t index = 0; index < count; ++index) {
        TopicConnection connection;
        connection.key = read_cursor(reader);
        connection.schema = reader.u64();
        connection.mid = reader.u64();
        if (!valid_cursor(connection.key) || connection.key.topic.empty() ||
            !cursor_before(previous, connection.key) || !connection.schema ||
            connection.mid == 0 || connection.mid >= central_owner) {
            return invalid();
        }

        previous = connection.key;
        page.connections.push_back(std::move(connection));
    }

    page.next = read_cursor(reader);
    if (!reader.done() || !valid_cursor(page.next) ||
        (!page.next.topic.empty() && (count == 0 || page.next != previous))) {
        return invalid();
    }

    return page;
}
}
