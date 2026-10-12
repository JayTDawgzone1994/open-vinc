#include <vinc/param/detail/protocol.hpp>

namespace vinc::param::detail {
Result<void> put_value(control::Bytes& bytes, const Value& value) {
    auto valid = validate_value(value);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    control::put_u32(bytes, static_cast<std::uint32_t>(type_of(value)));
    auto encoded = control::put_string(bytes, value_text(value));
    if (!encoded) {
        return std::unexpected(wrap_error(ErrorCode::InvalidValue, ErrorOperation::Encode, encoded.error()));
    }

    return {};
}

Result<Value> read_value(control::Reader& reader) {
    const auto type = static_cast<Type>(reader.u32());
    const auto text = reader.string();
    return parse_value(type, text);
}

Result<control::Bytes> encode_snapshot(const Snapshot& snapshot) {
    if (snapshot.revision == UINT64_MAX || snapshot.entries.size() > config::param_entry_limit) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Encode});
    }
    control::Bytes bytes;
    control::put_u32(bytes, version);
    control::put_u64(bytes, snapshot.revision);
    control::put_u32(bytes, static_cast<std::uint32_t>(snapshot.entries.size()));
    for (const auto& [name, entry] : snapshot.entries) {
        if (!valid_name(name)) {
            return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Encode});
        }

        auto named = control::put_string(bytes, name);
        if (!named) {
            return std::unexpected(wrap_error(ErrorCode::InvalidName, ErrorOperation::Encode, named.error()));
        }

        auto current = put_value(bytes, entry.value);
        if (!current) {
            return std::unexpected(current.error());
        }

        auto defaults = put_value(bytes, entry.default_value);
        if (!defaults) {
            return std::unexpected(defaults.error());
        }
        control::put_u32(bytes, static_cast<std::uint32_t>(entry.permission));
        control::put_u64(bytes, entry.version);
        control::put_u64(bytes, entry.timestamp);
    }
    if (bytes.size() > config::param_slot_bytes) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Encode});
    }

    return bytes;
}

Result<Snapshot> decode_snapshot(std::span<const std::byte> bytes) {
    control::Reader reader(bytes);
    if (reader.u32() != version) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Decode});
    }
    Snapshot snapshot;
    snapshot.revision = reader.u64();
    const auto count = reader.u32();
    if (snapshot.revision == UINT64_MAX || count > config::param_entry_limit ||
        bytes.size() > config::param_slot_bytes) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Decode});
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        auto name = reader.string();
        auto value = read_value(reader);
        auto defaults = read_value(reader);
        const auto permission = reader.u32();
        const auto revision = reader.u64();
        const auto timestamp = reader.u64();
        if (!valid_name(name) || !value || !defaults || type_of(*value) != type_of(*defaults) ||
            permission > 1 || !revision || revision > snapshot.revision || snapshot.entries.contains(name)) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Decode});
        }

        snapshot.entries.emplace(std::move(name),
                                 Entry{std::move(*value), std::move(*defaults),
                                       static_cast<Permission>(permission), revision, timestamp});
    }
    if (!reader.done()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Decode});
    }

    return snapshot;
}
}
