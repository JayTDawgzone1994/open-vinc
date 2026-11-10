#include <vinc/rpc/detail/address.hpp>
#include <vinc/rpc/detail/protocol.hpp>

namespace vinc::rpc::detail {
Result<Address> parse_address(std::string_view address) {
    const auto separator = address.find('.');
    if (separator == std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Create});
    }

    const auto module = address.substr(0, separator);
    const auto service = address.substr(separator + 1);
    // valid_name rejects further separators, NULs and non-ASCII bytes as well
    // as enforcing the existing per-segment RPC wire limit.
    if (!valid_name(module) || !valid_name(service)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Create});
    }

    return Address{std::string(module), std::string(service)};
}
}
