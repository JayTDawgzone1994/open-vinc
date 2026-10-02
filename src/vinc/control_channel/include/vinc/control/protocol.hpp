#pragma once

// Small framed control messages shared by the framework and module runtimes.
// Commands, lifecycle and Bus registration use this envelope; large message payloads do not.

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
enum class Kind : std::uint16_t {
    Hello = 1,
    Heartbeat,
    Request,
    Response,
    Event
};
enum class Domain : std::uint16_t {
    Session = 0,
    Lifecycle = 1,
    Command = 2,
    Bus = 3
};

// request correlates a response with one request; events use zero.
// domain selects the subsystem, and operation selects an action within that subsystem.
struct Frame {
    Kind kind = Kind::Event;
    std::uint16_t domain = 0;
    std::uint16_t operation = 0;
    std::uint64_t request = 0;
    std::vector<std::byte> payload;
};

using Bytes = std::vector<std::byte>;
// Existing encoding APIs throw std::length_error when a wire limit is exceeded.
// Error-interface migration is separate from the wire format and readability work.
Bytes encode(const Frame& frame);

class Decoder {
public:
    // Retain at most one incomplete frame; returned frames own their payloads.
    // After a decoding error, discard the decoder along with the failed connection.
    std::expected<std::vector<Frame>, std::string> feed(std::span<const std::byte> bytes);

private:
    Bytes buffer_;
};

void put_u32(Bytes& output, std::uint32_t value);
void put_u64(Bytes& output, std::uint64_t value);
void put_string(Bytes& output, std::string_view value);

// A failed read latches an invalid state. Always check done() after decoding
// the expected fields; it rejects both truncation and unexpected trailing bytes.
class Reader {
public:
    // Borrows the input; its storage must remain valid throughout decoding.
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {
    }

    std::uint32_t u32();
    std::uint64_t u64();
    std::string string();

    bool done() const {
        return valid_ && offset_ == bytes_.size();
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = true;
};

Bytes encode_arguments(const std::vector<std::string>& args);
std::expected<std::vector<std::string>, std::string> decode_arguments(std::span<const std::byte> data);
} // namespace vinc::control
