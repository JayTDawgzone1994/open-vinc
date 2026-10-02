// Encode fixed-width wire fields explicitly and decode arbitrary stream fragments.
// Length checks happen before buffering a payload; callers must reject malformed frames.

#include <vinc/control/protocol.hpp>
#include <algorithm>
#include <stdexcept>

namespace vinc::control {
namespace {
constexpr std::uint32_t magic = 0x56494e43;
void put_u16(Bytes& out, std::uint16_t n) {
    out.push_back(std::byte(n >> 8)); out.push_back(std::byte(n));
}
std::uint16_t u16(std::span<const std::byte> in, std::size_t p) {
    return std::uint16_t((std::to_integer<unsigned>(in[p]) << 8) | std::to_integer<unsigned>(in[p + 1]));
}
}
void put_u32(Bytes& out, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(std::byte(n >> shift));
}
void put_u64(Bytes& out, std::uint64_t n) {
    put_u32(out, std::uint32_t(n >> 32)); put_u32(out, std::uint32_t(n));
}
void put_string(Bytes& out, std::string_view value) {
    if (value.size() > max_payload || out.size() + 4 + value.size() > max_payload)
        throw std::length_error("control payload limit");
    put_u32(out, static_cast<std::uint32_t>(value.size()));
    for (unsigned char c : value) out.push_back(std::byte(c));
}
std::uint32_t Reader::u32() {
    if (!valid_ || bytes_.size() - offset_ < 4) { valid_ = false; return 0; }
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | std::to_integer<unsigned>(bytes_[offset_++]);
    return n;
}
std::uint64_t Reader::u64() { const auto hi = u32(); return (std::uint64_t(hi) << 32) | u32(); }
std::string Reader::string() {
    const auto size = u32();
    if (!valid_ || size > bytes_.size() - offset_) { valid_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size; return s;
}
// Wire layout: magic/version/kind, domain/operation, request ID, payload length, payload.
// Do not memcpy Frame: vector storage, padding and native byte order are process-local.
Bytes encode(const Frame& frame) {
    if (frame.payload.size() > max_payload) throw std::length_error("control payload limit");
    Bytes out; out.reserve(header_size + frame.payload.size());
    put_u32(out, magic); put_u16(out, 1); put_u16(out, static_cast<std::uint16_t>(frame.kind));
    put_u16(out, frame.domain); put_u16(out, frame.operation);
    put_u64(out, frame.request); put_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end()); return out;
}
// One receive can contain half a header, several frames, or a frame plus a tail.
// Accumulate only to the next boundary and validate the header before accepting its length.
std::expected<std::vector<Frame>, std::string> Decoder::feed(std::span<const std::byte> bytes) {
    std::vector<Frame> frames;
    while (!bytes.empty()) {
        std::size_t target = header_size;
        if (buffer_.size() >= header_size) {
            Reader length(std::span<const std::byte>(buffer_).subspan(20, 4));
            target += length.u32();
        }
        const auto n = (std::min)(target - buffer_.size(), bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + n); bytes = bytes.subspan(n);
        if (buffer_.size() < header_size) continue;
        const auto data = std::span<const std::byte>(buffer_);
        Reader prefix(data.first(4)); Reader length(data.subspan(20, 4));
        const auto size = length.u32(); const auto kind = u16(data, 6);
        if (prefix.u32() != magic || u16(data, 4) != 1 || kind < 1 || kind > 5 || size > max_payload)
            return std::unexpected("invalid control header or unsupported protocol version");
        if (buffer_.size() != header_size + size) continue;
        Reader request(data.subspan(12, 8));
        frames.push_back(Frame{static_cast<Kind>(kind), u16(data, 8), u16(data, 10), request.u64(),
            Bytes(buffer_.begin() + header_size, buffer_.end())});
        buffer_.clear();
    }
    return frames;
}
Bytes encode_arguments(const std::vector<std::string>& args) {
    if (args.size() > 256) throw std::length_error("too many arguments");
    Bytes out; put_u32(out, static_cast<std::uint32_t>(args.size()));
    for (const auto& arg : args) put_string(out, arg);
    return out;
}
std::expected<std::vector<std::string>, std::string> decode_arguments(std::span<const std::byte> data) {
    Reader reader(data); const auto count = reader.u32();
    if (count > 256) return std::unexpected("too many arguments");
    std::vector<std::string> args;
    for (std::uint32_t i = 0; i < count; ++i) args.push_back(reader.string());
    if (!reader.done()) return std::unexpected("invalid arguments");
    return args;
}
} // namespace vinc::control
