#pragma once
#include <vinc/param/types.hpp>
#include <vinc/param/detail/error_protocol.hpp>

namespace vinc::param::detail {
inline constexpr std::uint16_t domain = 4;
inline constexpr std::uint32_t version = 1;
// Largest request: version, name length, value type/length, permission and two bounded strings.
inline constexpr std::size_t request_max_bytes = 5 * 4 + config::param_name_bytes + config::param_string_bytes;

enum Operation : std::uint16_t {
    Attach = 0x0101,
    Register = 0x0102,
    Set = 0x0103,
    Changed = 0x0104,
    Watch = 0x0105
};

Result<void> put_value(control::Bytes& bytes, const Value& value);
Result<Value> read_value(control::Reader& reader);
Result<control::Bytes> encode_snapshot(const Snapshot& snapshot);
Result<Snapshot> decode_snapshot(std::span<const std::byte> bytes);
}
