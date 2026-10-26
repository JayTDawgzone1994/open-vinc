#include <vinc/rpc/detail/protocol.hpp>
#include <limits>

namespace vinc::rpc::detail {
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > config::rpc_name_bytes) {
        return false;
    }

    for (unsigned char character : name) {
        const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_' && character != '-') {
            return false;
        }
    }

    return true;
}

Result<std::uint64_t> slot_bytes(const ServiceOptions& options) {
    const auto limit = config::rpc_shared_bytes;
    if (options.slots == 0 || options.slots > config::rpc_slot_limit || options.request_bytes == 0 ||
        options.response_bytes == 0 || options.request_bytes > limit || options.response_bytes > limit) {
        return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Register});
    }

    // One aligned native publication word precedes each encoded byte image.
    return 16 + ((options.request_bytes + 7) & ~std::uint64_t{7}) +
           ((options.response_bytes + 7) & ~std::uint64_t{7});
}

Result<std::uint64_t> pool_bytes(const ServiceOptions& options) {
    auto stride = slot_bytes(options);
    if (!stride) {
        return std::unexpected(stride.error());
    }

    if (*stride > config::rpc_shared_bytes / options.slots) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Register});
    }

    return *stride * options.slots;
}

control::Bytes encode_transfer(const Transfer& value) {
    control::Bytes bytes;
    control::put_u32(bytes, version);
    control::put_u64(bytes, value.token);
    control::put_u64(bytes, value.service);
    (void)control::put_string(bytes, value.memory);
    control::put_u64(bytes, value.extent);
    control::put_u64(bytes, value.offset);
    control::put_u64(bytes, value.request_bytes);
    control::put_u64(bytes, value.response_bytes);
    control::put_u64(bytes, value.size);
    return bytes;
}

Result<Transfer> decode_transfer(std::span<const std::byte> bytes) {
    control::Reader reader(bytes);
    const auto wire_version = reader.u32();
    Transfer value;
    value.token = reader.u64();
    value.service = reader.u64();
    value.memory = reader.string();
    value.extent = reader.u64();
    value.offset = reader.u64();
    value.request_bytes = reader.u64();
    value.response_bytes = reader.u64();
    value.size = reader.u64();
    auto stride = slot_bytes({value.request_bytes, value.response_bytes, 1});
    if (!reader.done() || wire_version != version || !value.token || !value.service || value.memory.empty() ||
        value.memory.size() > 24 || !stride || value.extent > config::rpc_shared_bytes ||
        value.offset % *stride != 0 || value.extent % *stride != 0 || value.offset > value.extent ||
        *stride > value.extent - value.offset) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    return value;
}

control::Frame frame(Operation operation, std::uint64_t request, control::Bytes bytes) {
    return {control::Kind::Request, domain, operation, request, std::move(bytes)};
}
}
