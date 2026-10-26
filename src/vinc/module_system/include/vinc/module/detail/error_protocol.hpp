#pragma once

// Module v3 payloads. Error records are numeric big-endian fields, never C++ layout.
#include <vinc/module/error.hpp>
#include <vinc/control/protocol.hpp>
#include <algorithm>
#include <array>
#include <bit>

namespace vinc::detail {
inline constexpr std::uint32_t error_wire_version = 1;
inline constexpr std::size_t error_wire_max_records = 16;
inline constexpr std::size_t error_wire_header_bytes = 12;
inline constexpr std::size_t error_wire_record_bytes = 24;
inline constexpr std::size_t result_tag_bytes = 4;
inline constexpr std::uint32_t result_success = 0;
inline constexpr std::uint32_t result_failure = 1;

inline Error module_protocol_error() {
    return Error{module_system::ErrorCode::Protocol, module_system::ErrorOperation::Receive};
}

inline void encode_error_record(control::Bytes& bytes, const ErrorRecord& record) {
    control::put_u32(bytes, static_cast<std::uint32_t>(record.domain));
    control::put_u32(bytes, std::bit_cast<std::uint32_t>(record.code));
    control::put_u32(bytes, std::bit_cast<std::uint32_t>(record.operation));
    control::put_u32(bytes, static_cast<std::uint32_t>(record.native.source));
    control::put_u64(bytes, std::bit_cast<std::uint64_t>(record.native.code));
}

inline control::Bytes encode_error(const Error& error) {
    const auto causes = error.causes();
    const auto count = (std::min)(causes.size() + 1, error_wire_max_records);
    const bool truncated = error.truncated() || count < causes.size() + 1;
    control::Bytes bytes;
    bytes.reserve(error_wire_header_bytes + count * error_wire_record_bytes);
    control::put_u32(bytes, error_wire_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(count));
    control::put_u32(bytes, truncated ? 1 : 0);
    encode_error_record(bytes, error.record());
    for (std::size_t index = 1; index < count; ++index) {
        // The final retained record is always the deepest cause, including on truncation.
        const auto& record = index + 1 == count ? causes.back() : causes[index - 1];
        encode_error_record(bytes, record);
    }
    return bytes;
}

inline Result<Error> decode_error(std::span<const std::byte> bytes) {
    if (bytes.size() < error_wire_header_bytes) {
        return std::unexpected(module_protocol_error());
    }

    control::Reader reader(bytes);
    const auto version = reader.u32();
    const auto count = reader.u32();
    const auto flags = reader.u32();
    const bool invalid_count = count == 0 || count > error_wire_max_records;
    if (version != error_wire_version || invalid_count || flags > 1 || (flags != 0 && count < 2) ||
        bytes.size() != error_wire_header_bytes + count * error_wire_record_bytes) {
        return std::unexpected(module_protocol_error());
    }

    std::array<ErrorRecord, error_wire_max_records> records{};
    for (std::size_t index = 0; index < count; ++index) {
        auto& record = records[index];
        record.domain = static_cast<ErrorDomain>(reader.u32());
        record.code = std::bit_cast<std::int32_t>(reader.u32());
        record.operation = std::bit_cast<std::int32_t>(reader.u32());
        record.native.source = static_cast<NativeErrorSource>(reader.u32());
        record.native.code = std::bit_cast<std::int64_t>(reader.u64());
    }
    if (!reader.done()) {
        return std::unexpected(module_protocol_error());
    }

    const auto& current = records.front();
    return Error::from_raw(current.domain, current.code, current.operation, current.native,
                           std::span<const ErrorRecord>(records).subspan(1, count - 1), flags != 0);
}

inline control::Bytes encode_failure(const Error& error) {
    control::Bytes bytes;
    control::put_u32(bytes, result_failure);
    const auto record = encode_error(error);
    bytes.insert(bytes.end(), record.begin(), record.end());
    return bytes;
}

// A successful response owns its payload. A failed response owns its cause chain.
inline Result<control::Bytes> decode_response(std::span<const std::byte> bytes) {
    if (bytes.size() < result_tag_bytes) {
        return std::unexpected(module_protocol_error());
    }

    control::Reader tag(bytes.first(result_tag_bytes));
    const auto kind = tag.u32();
    const auto payload = bytes.subspan(result_tag_bytes);
    if (kind == result_success) {
        return control::Bytes(payload.begin(), payload.end());
    }
    if (kind != result_failure) {
        return std::unexpected(module_protocol_error());
    }

    auto error = decode_error(payload);
    if (!error) {
        return std::unexpected(error.error());
    }
    if (error->domain != ErrorDomain::ModuleSystem) {
        return std::unexpected(module_protocol_error());
    }

    return std::unexpected(*error);
}
}
