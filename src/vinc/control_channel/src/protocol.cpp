// Encode fixed-width wire fields explicitly and decode arbitrary stream fragments.
// Length checks happen before buffering a payload; callers must reject malformed frames.

#include <vinc/control/protocol.hpp>
#include <algorithm>
#include <stdexcept>

namespace vinc::control {
namespace {
// Version 1 wire offsets and widths are protocol constants, not runtime settings.
constexpr std::uint32_t magic = 0x56494e43;
constexpr std::uint16_t protocol_version = 1;
constexpr std::size_t u32_bytes = 4;
constexpr std::size_t u64_bytes = 8;
constexpr std::size_t version_offset = 4;
constexpr std::size_t kind_offset = 6;
constexpr std::size_t domain_offset = 8;
constexpr std::size_t operation_offset = 10;
constexpr std::size_t request_offset = 12;
constexpr std::size_t payload_length_offset = 20;
constexpr std::size_t argument_count_limit = 256;

void put_u16(Bytes& output, std::uint16_t value) {
    output.push_back(std::byte(value >> 8));
    output.push_back(std::byte(value));
}

std::uint16_t u16(std::span<const std::byte> input, std::size_t offset) {
    return std::uint16_t((std::to_integer<unsigned>(input[offset]) << 8) |
                         std::to_integer<unsigned>(input[offset + 1]));
}
}

void put_u32(Bytes& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(std::byte(value >> shift));
    }
}

void put_u64(Bytes& output, std::uint64_t value) {
    put_u32(output, std::uint32_t(value >> 32));
    put_u32(output, std::uint32_t(value));
}

void put_string(Bytes& output, std::string_view value) {
    if (value.size() > max_payload || output.size() + u32_bytes + value.size() > max_payload) {
        throw std::length_error("control payload limit");
    }

    put_u32(output, static_cast<std::uint32_t>(value.size()));
    for (unsigned char byte : value) {
        output.push_back(std::byte(byte));
    }
}

std::uint32_t Reader::u32() {
    if (!valid_ || bytes_.size() - offset_ < u32_bytes) {
        valid_ = false;
        return 0;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < u32_bytes; ++i) {
        value = (value << 8) | std::to_integer<unsigned>(bytes_[offset_++]);
    }
    return value;
}

std::uint64_t Reader::u64() {
    const auto high_word = u32();
    return (std::uint64_t(high_word) << 32) | u32();
}

std::string Reader::string() {
    const auto size = u32();
    if (!valid_ || size > bytes_.size() - offset_) {
        valid_ = false;
        return {};
    }

    // Wire strings are contiguous bytes. char may inspect their object representation;
    // std::string copies the checked range before the borrowed input can expire.
    std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return value;
}

// Wire layout: magic/version/kind, domain/operation, request ID, payload length, payload.
// Do not memcpy Frame: vector storage, padding and native byte order are process-local.
Bytes encode(const Frame& frame) {
    if (frame.payload.size() > max_payload) {
        throw std::length_error("control payload limit");
    }

    Bytes output;
    output.reserve(header_size + frame.payload.size());
    put_u32(output, magic);
    put_u16(output, protocol_version);
    put_u16(output, static_cast<std::uint16_t>(frame.kind));
    put_u16(output, frame.domain);
    put_u16(output, frame.operation);
    put_u64(output, frame.request);
    put_u32(output, static_cast<std::uint32_t>(frame.payload.size()));
    output.insert(output.end(), frame.payload.begin(), frame.payload.end());
    return output;
}

// One receive can contain half a header, several frames, or a frame plus a tail.
// Accumulate only to the next boundary and validate the header before accepting its length.
std::expected<std::vector<Frame>, std::string> Decoder::feed(std::span<const std::byte> bytes) {
    std::vector<Frame> frames;
    while (!bytes.empty()) {
        std::size_t target = header_size;
        if (buffer_.size() >= header_size) {
            Reader length(std::span<const std::byte>(buffer_).subspan(payload_length_offset, u32_bytes));
            target += length.u32();
        }
        const auto bytes_to_copy = (std::min)(target - buffer_.size(), bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + bytes_to_copy);
        bytes = bytes.subspan(bytes_to_copy);
        if (buffer_.size() < header_size) {
            continue;
        }

        const auto data = std::span<const std::byte>(buffer_);
        Reader prefix(data.first(u32_bytes));
        Reader length(data.subspan(payload_length_offset, u32_bytes));
        const auto size = length.u32();
        const auto kind = u16(data, kind_offset);
        if (prefix.u32() != magic || u16(data, version_offset) != protocol_version ||
            kind < static_cast<std::uint16_t>(Kind::Hello) ||
            kind > static_cast<std::uint16_t>(Kind::Event) || size > max_payload) {
            return std::unexpected("invalid control header or unsupported protocol version");
        }

        if (buffer_.size() != header_size + size) {
            continue;
        }

        Reader request(data.subspan(request_offset, u64_bytes));
        frames.push_back(Frame{static_cast<Kind>(kind), u16(data, domain_offset), u16(data, operation_offset),
                               request.u64(), Bytes(buffer_.begin() + header_size, buffer_.end())});
        buffer_.clear();
    }
    return frames;
}

Bytes encode_arguments(const std::vector<std::string>& args) {
    if (args.size() > argument_count_limit) {
        throw std::length_error("too many arguments");
    }

    Bytes output;
    put_u32(output, static_cast<std::uint32_t>(args.size()));
    for (const auto& arg : args) {
        put_string(output, arg);
    }
    return output;
}

std::expected<std::vector<std::string>, std::string> decode_arguments(std::span<const std::byte> data) {
    Reader reader(data);
    const auto count = reader.u32();
    if (count > argument_count_limit) {
        return std::unexpected("too many arguments");
    }

    std::vector<std::string> args;
    for (std::uint32_t i = 0; i < count; ++i) {
        args.push_back(reader.string());
    }
    if (!reader.done()) {
        return std::unexpected("invalid arguments");
    }

    return args;
}
} // namespace vinc::control
