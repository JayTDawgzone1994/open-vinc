#pragma once

// Portable address values; native sockaddr conversion stays in the implementation.
// Numeric parsing is local work. Hostname resolution is a separate blocking operation.

#include <vinc/osal/error.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vinc::osal {
enum class AddressFamily {
    IPv4,
    IPv6,
    Local
};
enum class SocketType {
    Stream,
    Datagram
};

struct IPv4Address {
    std::array<std::uint8_t, 4> bytes{};
    static Result<IPv4Address> parse(std::string_view);
    std::string to_string() const;

    static constexpr IPv4Address loopback() noexcept {
        return {{127, 0, 0, 1}};
    }

    friend bool operator==(const IPv4Address&, const IPv4Address&) = default;
};

struct IPv6Address {
    std::array<std::uint8_t, 16> bytes{};
    std::uint32_t scope_id = 0;
    // Numeric address only; scope is an explicit numeric argument, not DNS.
    static Result<IPv6Address> parse(std::string_view, std::uint32_t scope_id = 0);
    std::string to_string() const;

    static constexpr IPv6Address loopback() noexcept {
        IPv6Address address;
        address.bytes[15] = 1;
        return address;
    }

    friend bool operator==(const IPv6Address&, const IPv6Address&) = default;
};

struct IpEndpoint {
    std::variant<IPv4Address, IPv6Address> address;
    std::uint16_t port = 0; // host byte order
    friend bool operator==(const IpEndpoint&, const IpEndpoint&) = default;
};

struct LocalEndpoint {
    std::string path; // UTF-8 pathname; empty represents an unnamed queried peer
    friend bool operator==(const LocalEndpoint&, const LocalEndpoint&) = default;
};

using SocketAddress = std::variant<IpEndpoint, LocalEndpoint>;
AddressFamily address_family(const SocketAddress&) noexcept;
// Explicit, synchronous OS resolver. No deadline/cancellation guarantee and no
// automatic fallback/racing between returned addresses. Never resolves Local.
Result<std::vector<IpEndpoint>> resolve_host(std::string_view host, std::uint16_t port,
                                             SocketType type = SocketType::Stream,
                                             std::optional<AddressFamily> family = {});
} // namespace vinc::osal
