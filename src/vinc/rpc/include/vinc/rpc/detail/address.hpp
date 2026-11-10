#pragma once
#include <vinc/rpc/error.hpp>
#include <string>
#include <string_view>

namespace vinc::rpc::detail {
struct Address {
    std::string module;
    std::string service;
};

// Own the parsed segments; clients must not borrow the factory argument.
Result<Address> parse_address(std::string_view address);
}
