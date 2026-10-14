#pragma once
#include <vinc/rpc/types.hpp>
#include <vinc/rpc/detail/error_protocol.hpp>

namespace vinc::rpc::detail {
inline constexpr std::uint16_t domain = 6;
inline constexpr std::uint32_t version = 1;

enum Operation : std::uint16_t {
    Register = 1,
    Acquire = 2,
    Grant = 3,
    Submit = 4,
    Invoke = 5,
    Complete = 6,
    Reply = 7,
    Release = 8
};

// Numeric wire descriptor, never a process-local address.
struct Transfer {
    std::uint64_t token = 0;
    std::uint64_t service = 0;
    std::string memory;
    std::uint64_t extent = 0;
    std::uint64_t offset = 0;
    std::uint64_t request_bytes = 0;
    std::uint64_t response_bytes = 0;
    std::uint64_t size = 0;
};

bool valid_name(std::string_view name);
Result<std::uint64_t> slot_bytes(const ServiceOptions& options);
Result<std::uint64_t> pool_bytes(const ServiceOptions& options);
control::Bytes encode_transfer(const Transfer& value);
Result<Transfer> decode_transfer(std::span<const std::byte> bytes);
control::Frame frame(Operation operation, std::uint64_t request, control::Bytes bytes = {});
}
