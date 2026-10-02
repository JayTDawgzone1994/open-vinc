#pragma once

// Move-only nonblocking sockets with optional deadline-based convenience calls.
// Stream operations may transfer only part of a buffer; higher layers assemble messages.

#include <vinc/osal/socket_address.hpp>
#include <concepts>
#include <cstddef>
#include <memory>
#include <span>
#include <stop_token>
#include <type_traits>

namespace vinc::osal {
enum class ConnectProgress { Connected, InProgress };
enum class ShutdownDirection { Read, Write, Both };
struct ReceiveResult {
    std::size_t bytes = 0;
    bool eof = false; // only for streams, never for an empty datagram
    bool truncated = false; // datagram tail was discarded
};
struct DatagramResult {
    ReceiveResult data;
    SocketAddress sender;
};
namespace detail {
struct SocketState;
enum class SocketOptionKey {
    ReceiveBuffer, SendBuffer, NoDelay, KeepAlive, IPv6Only, Broadcast,
    ReuseAddress, ExclusiveAddressUse, MulticastLoop, MulticastHops
};
}
namespace socket_options {
template<detail::SocketOptionKey Key, class T> struct Option {
    using value_type = T;
    static constexpr auto key = Key;
};
using ReceiveBuffer = Option<detail::SocketOptionKey::ReceiveBuffer, int>;
using SendBuffer = Option<detail::SocketOptionKey::SendBuffer, int>;
using NoDelay = Option<detail::SocketOptionKey::NoDelay, bool>;
using KeepAlive = Option<detail::SocketOptionKey::KeepAlive, bool>;
using IPv6Only = Option<detail::SocketOptionKey::IPv6Only, bool>;
using Broadcast = Option<detail::SocketOptionKey::Broadcast, bool>;
// POSIX SO_REUSEADDR semantics; Unsupported on Windows (not the same policy).
using ReuseAddress = Option<detail::SocketOptionKey::ReuseAddress, bool>;
// Windows exclusive binding; Unsupported on POSIX. Configure before bind.
using ExclusiveAddressUse = Option<detail::SocketOptionKey::ExclusiveAddressUse, bool>;
using MulticastLoop = Option<detail::SocketOptionKey::MulticastLoop, bool>;
using MulticastHops = Option<detail::SocketOptionKey::MulticastHops, int>;
}
template<class T> concept SocketOption = requires {
    typename T::value_type;
    { T::key } -> std::convertible_to<detail::SocketOptionKey>;
} && (std::same_as<typename T::value_type, int> || std::same_as<typename T::value_type, bool>);

class SocketPoller;
class Socket {
public:
    Socket() noexcept = default;
    ~Socket();
    Socket(Socket&&) noexcept;
    Socket& operator=(Socket&&) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] static Result<Socket> open(AddressFamily, SocketType);
    explicit operator bool() const noexcept;
    [[nodiscard]] Result<void> bind(const SocketAddress&);
    [[nodiscard]] Result<void> listen(int backlog);
    // try_* never wait for readiness: WouldBlock asks the caller to poll and retry.
    [[nodiscard]] Result<Socket> try_accept();
    [[nodiscard]] Result<ConnectProgress> try_connect(const SocketAddress&);
    [[nodiscard]] Result<ConnectProgress> finish_connect();
    [[nodiscard]] Result<std::size_t> try_send(std::span<const std::byte>);
    [[nodiscard]] Result<ReceiveResult> try_receive(std::span<std::byte>);
    [[nodiscard]] Result<std::size_t> try_send_to(std::span<const std::byte>, const SocketAddress&);
    [[nodiscard]] Result<DatagramResult> try_receive_from(std::span<std::byte>);
    [[nodiscard]] Result<Socket> accept_until(Deadline, std::stop_token = {});
    // Timeout/cancellation/failure closes a stream connection attempt. Check
    // bool() before reuse; create a new socket for a new attempt.
    [[nodiscard]] Result<void> connect_until(const SocketAddress&, Deadline, std::stop_token = {});
    [[nodiscard]] Result<std::size_t> send_until(std::span<const std::byte>, Deadline, std::stop_token = {});
    [[nodiscard]] Result<ReceiveResult> receive_until(std::span<std::byte>, Deadline, std::stop_token = {});
    [[nodiscard]] Result<std::size_t> send_to_until(std::span<const std::byte>, const SocketAddress&, Deadline, std::stop_token = {});
    [[nodiscard]] Result<DatagramResult> receive_from_until(std::span<std::byte>, Deadline, std::stop_token = {});
    [[nodiscard]] Result<void> shutdown(ShutdownDirection);
    [[nodiscard]] Result<void> close(); // idempotent; unregister from pollers first
    [[nodiscard]] Result<SocketAddress> local_address() const;
    [[nodiscard]] Result<SocketAddress> peer_address() const;
    // Consumes SO_ERROR. During/after a failed connect use finish_connect()
    // instead, so querying diagnostics cannot erase the connection result.
    [[nodiscard]] Result<std::optional<OsError>> take_pending_error();
    template<SocketOption Option> Result<void> set_option(typename Option::value_type value) {
        return set_option_value(Option::key, static_cast<int>(value));
    }
    template<SocketOption Option> Result<typename Option::value_type> get_option() const {
        auto value = get_option_value(Option::key);
        if (!value) return std::unexpected(value.error());
        return static_cast<typename Option::value_type>(*value);
    }
    Result<void> set_multicast_interface(IPv4Address);
    Result<void> set_multicast_interface(std::uint32_t ipv6_index);
    Result<IPv4Address> multicast_interface_v4() const;
    Result<std::uint32_t> multicast_interface_v6() const;
    Result<void> join_multicast(IPv4Address group, IPv4Address interface_address = {});
    Result<void> leave_multicast(IPv4Address group, IPv4Address interface_address = {});
    Result<void> join_multicast(IPv6Address group, std::uint32_t interface_index);
    Result<void> leave_multicast(IPv6Address group, std::uint32_t interface_index);
private:
    friend class SocketPoller;
    explicit Socket(std::shared_ptr<detail::SocketState>);
    Result<void> set_option_value(detail::SocketOptionKey, int);
    Result<int> get_option_value(detail::SocketOptionKey) const;
    std::shared_ptr<detail::SocketState> state_;
};
} // namespace vinc::osal
