#pragma once

#include <cstdint>

namespace vinc::message::detail::format {
// Version 2 is an explicitly encoded little-endian layout, never a native C++ struct.
// Offsets and widths are protocol constants, not configurable storage limits.
inline constexpr std::uint64_t magic = 0x00525241434e4956ULL; // "VINCARR\0"
inline constexpr std::uint32_t version = 2;
inline constexpr std::uint32_t compact_flag = 1;
inline constexpr std::uint32_t reserved_capacity_flag = 2;
inline constexpr unsigned integer_bytes = 8;
inline constexpr unsigned tag_bytes = 4;
inline constexpr std::uint64_t alignment = 8;
inline constexpr std::uint32_t boolean_scalar_code = 11;

namespace header {
inline constexpr std::uint64_t size = 64;
inline constexpr std::uint64_t magic = 0;
inline constexpr std::uint64_t version = 8;
inline constexpr std::uint64_t flags = 12;
inline constexpr std::uint64_t total_bytes = 16;
inline constexpr std::uint64_t node_count = 24;
inline constexpr std::uint64_t root_count = 32;
inline constexpr std::uint64_t descriptor_table = 40;
inline constexpr std::uint64_t payload = 48;
inline constexpr std::uint64_t reserved = 56;
}

namespace descriptor {
inline constexpr std::uint64_t size = 48;
inline constexpr std::uint64_t payload = 0;
inline constexpr std::uint64_t element_count = 8;
inline constexpr std::uint64_t scalar = 16;
inline constexpr std::uint64_t depth = 20;
inline constexpr std::uint64_t capacity = 24;
inline constexpr std::uint64_t schema_hash = 32;
inline constexpr std::uint64_t reserved = 40;
}
}
