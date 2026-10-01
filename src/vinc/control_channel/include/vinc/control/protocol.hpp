#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vinc::control {
// Wire integers are big endian. Never transmit C++ object representations.
constexpr std::size_t header_size = 24;
constexpr std::size_t max_payload = 16 * 1024;
enum class Kind : std::uint16_t { Hello = 1, Heartbeat, Request, Response, Event };
enum class Domain : std::uint16_t { Session = 0, Lifecycle = 1, Command = 2, Bus = 3 };
struct Frame {
    Kind kind = Kind::Event;
    std::uint16_t domain = 0;
    std::uint16_t operation = 0;
    std::uint64_t request = 0;
    std::vector<std::byte> payload;
};
using Bytes = std::vector<std::byte>;
Bytes encode(const Frame&);
class Decoder {
public:
    // At most one frame is buffered. A callback must not retain references.
    std::expected<std::vector<Frame>, std::string> feed(std::span<const std::byte>);
private:
    Bytes buffer_;
};
void put_u32(Bytes&, std::uint32_t);
void put_u64(Bytes&, std::uint64_t);
void put_string(Bytes&, std::string_view);
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::uint32_t u32();
    std::uint64_t u64();
    std::string string();
    bool done() const { return valid_ && offset_ == bytes_.size(); }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = true;
};
Bytes encode_arguments(const std::vector<std::string>&);
std::expected<std::vector<std::string>, std::string> decode_arguments(std::span<const std::byte>);
} // namespace vinc::control
