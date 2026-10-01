// Convert portable IPv4, IPv6 and local endpoints to and from native addresses.
// Explicit hostname resolution is kept separate from nonblocking socket I/O.

#include "socket_native.hpp"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <new>

namespace vinc::osal::detail {
namespace {
bool valid_utf8(std::string_view text) noexcept {
    std::uint32_t value = 0, minimum = 0;
    unsigned remaining = 0;
    for (const unsigned char byte : text) {
        if (!remaining) {
            if (byte < 0x80) continue;
            if (byte >= 0xc2 && byte <= 0xdf) { value = byte & 0x1f; minimum = 0x80; remaining = 1; }
            else if (byte >= 0xe0 && byte <= 0xef) { value = byte & 0x0f; minimum = 0x800; remaining = 2; }
            else if (byte >= 0xf0 && byte <= 0xf4) { value = byte & 7; minimum = 0x10000; remaining = 3; }
            else return false;
        } else {
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
            if (--remaining == 0 && (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))) return false;
        }
    }
    return remaining == 0;
}
}
SocketRuntime::~SocketRuntime() {
#if defined(_WIN32)
    WSACleanup();
#endif
}
Result<std::shared_ptr<SocketRuntime>> socket_runtime() {
    static std::mutex mutex;
    static std::weak_ptr<SocketRuntime> current;
    std::lock_guard lock(mutex);
    if (auto runtime = current.lock()) return runtime;
    // Allocate first; a failed WSAStartup must not trigger WSACleanup.
    auto* storage = ::operator new(sizeof(SocketRuntime));
#if defined(_WIN32)
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error) { ::operator delete(storage); return std::unexpected(socket_error(Operation::InitializeService, error)); }
#endif
    auto runtime = std::shared_ptr<SocketRuntime>(new (storage) SocketRuntime);
    current = runtime;
    return runtime;
}
OsError socket_error(Operation operation, int error) noexcept {
    ErrorCode code = ErrorCode::SystemError;
#if defined(_WIN32)
    switch (error) {
    case WSAEWOULDBLOCK: code = ErrorCode::WouldBlock; break;
    case WSAEADDRINUSE: code = ErrorCode::AddressInUse; break;
    case WSAEADDRNOTAVAIL: code = ErrorCode::AddressNotAvailable; break;
    case WSAECONNREFUSED: code = ErrorCode::ConnectionRefused; break;
    case WSAECONNRESET: case WSAECONNABORTED: code = ErrorCode::ConnectionReset; break;
    case WSAENOTCONN: case WSAEDESTADDRREQ: code = ErrorCode::NotConnected; break;
    case WSAESHUTDOWN: code = ErrorCode::PeerClosed; break;
    case WSAEMSGSIZE: code = ErrorCode::MessageTooLarge; break;
    case WSAENETUNREACH: case WSAEHOSTUNREACH: case WSAENETDOWN: code = ErrorCode::NetworkUnreachable; break;
    case WSAEAFNOSUPPORT: case WSAEPROTONOSUPPORT: case WSAESOCKTNOSUPPORT: case WSAEOPNOTSUPP:
    case WSAENOPROTOOPT: code = ErrorCode::Unsupported; break;
    case WSAEACCES: code = ErrorCode::PermissionDenied; break;
    case WSAEINVAL: case WSAEFAULT: code = ErrorCode::InvalidArgument; break;
    case WSAENOTSOCK: code = ErrorCode::Closed; break;
    case WSAENOBUFS: case WSAEMFILE: code = ErrorCode::ResourceExhausted; break;
    case WSAETIMEDOUT: code = ErrorCode::Timeout; break;
    case WSAHOST_NOT_FOUND: case WSANO_DATA: code = ErrorCode::NotFound; break;
    }
    return {code, operation, {error, std::system_category()}};
#else
    switch (error) {
    case EAGAIN: code = ErrorCode::WouldBlock; break;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: code = ErrorCode::WouldBlock; break;
#endif
    case EADDRINUSE: code = ErrorCode::AddressInUse; break;
    case EADDRNOTAVAIL: code = ErrorCode::AddressNotAvailable; break;
    case ECONNREFUSED: code = ErrorCode::ConnectionRefused; break;
    case ECONNRESET: case ECONNABORTED: code = ErrorCode::ConnectionReset; break;
    case ENOTCONN: case EDESTADDRREQ: code = ErrorCode::NotConnected; break;
    case EPIPE: code = ErrorCode::PeerClosed; break;
    case EMSGSIZE: code = ErrorCode::MessageTooLarge; break;
    case ENETUNREACH: case EHOSTUNREACH: case ENETDOWN: code = ErrorCode::NetworkUnreachable; break;
    case EAFNOSUPPORT: case EPROTONOSUPPORT: case EOPNOTSUPP: case ENOPROTOOPT: code = ErrorCode::Unsupported; break;
    case EACCES: case EPERM: code = ErrorCode::PermissionDenied; break;
    case EINVAL: case EFAULT: case ENAMETOOLONG: code = ErrorCode::InvalidArgument; break;
    case EBADF: case ENOTSOCK: code = ErrorCode::Closed; break;
    case ENOBUFS: case ENOMEM: case EMFILE: case ENFILE: code = ErrorCode::ResourceExhausted; break;
    case ETIMEDOUT: code = ErrorCode::Timeout; break;
    case ENOENT: code = ErrorCode::NotFound; break;
    }
    return {code, operation, {error, std::generic_category()}};
#endif
}
int native_family(AddressFamily family) noexcept {
    switch (family) { case AddressFamily::IPv4: return AF_INET; case AddressFamily::IPv6: return AF_INET6; case AddressFamily::Local: return AF_UNIX; }
    return AF_UNSPEC;
}
int native_type(SocketType type) noexcept { return type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM; }
Result<void> isolate_socket_handle(SocketHandle& handle, Operation operation) {
#if !defined(_WIN32)
    if (handle >= 0 && handle < 3) {
        const int replacement = fcntl(handle, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) return std::unexpected(socket_error(operation));
        ::close(handle); handle = replacement;
    }
#else
    (void)handle; (void)operation;
#endif
    return {};
}
Result<void> configure_socket(SocketHandle& socket) {
    auto isolated = isolate_socket_handle(socket, Operation::CreateSocket);
    if (!isolated) return isolated;
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return std::unexpected(socket_error(Operation::CreateSocket));
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0))
        return std::unexpected(OsError{ErrorCode::SystemError, Operation::CreateSocket, {static_cast<int>(GetLastError()), std::system_category()}});
#else
    const int flags = fcntl(socket, F_GETFL);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(socket, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(socket_error(Operation::CreateSocket));
#if defined(VINC_OSAL_MACOS)
    const int one = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0)
        return std::unexpected(socket_error(Operation::CreateSocket));
#endif
#endif
    return {};
}
Result<NativeAddress> encode_address(const SocketAddress& address, Operation operation) {
    NativeAddress native;
    if (auto ip = std::get_if<IpEndpoint>(&address)) {
        if (auto v4 = std::get_if<IPv4Address>(&ip->address)) {
            sockaddr_in value{}; value.sin_family = AF_INET; value.sin_port = htons(ip->port);
            std::memcpy(&value.sin_addr, v4->bytes.data(), 4);
            std::memcpy(&native.storage, &value, sizeof(value)); native.size = sizeof(value);
        } else {
            const auto& v6 = std::get<IPv6Address>(ip->address);
            sockaddr_in6 value{}; value.sin6_family = AF_INET6; value.sin6_port = htons(ip->port); value.sin6_scope_id = v6.scope_id;
            std::memcpy(&value.sin6_addr, v6.bytes.data(), 16);
            std::memcpy(&native.storage, &value, sizeof(value)); native.size = sizeof(value);
        }
    } else {
        const auto& path = std::get<LocalEndpoint>(address).path;
        sockaddr_un value{};
        if (path.empty() || path.find('\0') != std::string::npos || path.size() >= sizeof(value.sun_path) || !valid_utf8(path))
            return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
        value.sun_family = AF_UNIX;
        std::memcpy(value.sun_path, path.c_str(), path.size() + 1);
        native.size = static_cast<AddressLength>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(VINC_OSAL_MACOS)
        value.sun_len = static_cast<unsigned char>(native.size);
#endif
        std::memcpy(&native.storage, &value, sizeof(value));
    }
    return native;
}
Result<SocketAddress> decode_address(const sockaddr* address, AddressLength length, Operation operation) {
    if (length < static_cast<AddressLength>(sizeof(address->sa_family)))
        return std::unexpected(OsError{ErrorCode::SystemError, operation});
    if (address->sa_family == AF_INET && length >= static_cast<AddressLength>(sizeof(sockaddr_in))) {
        const auto* value = reinterpret_cast<const sockaddr_in*>(address);
        IPv4Address ip; std::memcpy(ip.bytes.data(), &value->sin_addr, 4);
        return SocketAddress{IpEndpoint{ip, ntohs(value->sin_port)}};
    }
    if (address->sa_family == AF_INET6 && length >= static_cast<AddressLength>(sizeof(sockaddr_in6))) {
        const auto* value = reinterpret_cast<const sockaddr_in6*>(address);
        IPv6Address ip; std::memcpy(ip.bytes.data(), &value->sin6_addr, 16); ip.scope_id = value->sin6_scope_id;
        return SocketAddress{IpEndpoint{ip, ntohs(value->sin6_port)}};
    }
    if (address->sa_family == AF_UNIX) {
        const auto* value = reinterpret_cast<const sockaddr_un*>(address);
        const auto count = length > static_cast<AddressLength>(offsetof(sockaddr_un, sun_path))
            ? std::min<std::size_t>(static_cast<std::size_t>(length) - offsetof(sockaddr_un, sun_path), sizeof(value->sun_path)) : 0;
        const auto end = std::find(value->sun_path, value->sun_path + count, '\0');
        return SocketAddress{LocalEndpoint{std::string(value->sun_path, end)}};
    }
    return std::unexpected(OsError{ErrorCode::Unsupported, operation});
}
} // namespace vinc::osal::detail

namespace vinc::osal {
Result<IPv4Address> IPv4Address::parse(std::string_view text) {
    if (text.empty() || text.size() >= INET_ADDRSTRLEN || text.find('\0') != std::string_view::npos)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::ParseAddress});
    char buffer[INET_ADDRSTRLEN]{}; std::memcpy(buffer, text.data(), text.size());
    IPv4Address result;
    if (inet_pton(AF_INET, buffer, result.bytes.data()) != 1)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::ParseAddress});
    return result;
}
Result<IPv6Address> IPv6Address::parse(std::string_view text, std::uint32_t scope) {
    if (text.empty() || text.size() >= INET6_ADDRSTRLEN || text.find('\0') != std::string_view::npos)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::ParseAddress});
    char buffer[INET6_ADDRSTRLEN]{}; std::memcpy(buffer, text.data(), text.size());
    IPv6Address result; result.scope_id = scope;
    if (inet_pton(AF_INET6, buffer, result.bytes.data()) != 1)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::ParseAddress});
    return result;
}
std::string IPv4Address::to_string() const {
    char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, bytes.data(), text, sizeof(text)); return text;
}
std::string IPv6Address::to_string() const {
    char text[INET6_ADDRSTRLEN]{}; inet_ntop(AF_INET6, bytes.data(), text, sizeof(text)); return text;
}
AddressFamily address_family(const SocketAddress& value) noexcept {
    if (auto ip = std::get_if<IpEndpoint>(&value))
        return std::holds_alternative<IPv4Address>(ip->address) ? AddressFamily::IPv4 : AddressFamily::IPv6;
    return AddressFamily::Local;
}
Result<std::vector<IpEndpoint>> resolve_host(std::string_view host, std::uint16_t port, SocketType type, std::optional<AddressFamily> family) {
    if (host.empty() || host.find('\0') != std::string_view::npos ||
        (family && *family != AddressFamily::IPv4 && *family != AddressFamily::IPv6) ||
        (type != SocketType::Stream && type != SocketType::Datagram))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::ResolveAddress});
    auto runtime = detail::socket_runtime();
    if (!runtime) return std::unexpected(runtime.error());
    addrinfo hints{}; hints.ai_family = family ? detail::native_family(*family) : AF_UNSPEC;
    hints.ai_socktype = detail::native_type(type); hints.ai_flags = AI_NUMERICSERV;
    const std::string name(host), service = std::to_string(port);
    addrinfo* raw = nullptr;
    const int error = getaddrinfo(name.c_str(), service.c_str(), &hints, &raw);
    if (error) {
#if defined(_WIN32)
        return std::unexpected(detail::socket_error(Operation::ResolveAddress, error));
#else
        if (error == EAI_SYSTEM) return std::unexpected(detail::socket_error(Operation::ResolveAddress));
        class ResolverCategory final : public std::error_category {
            const char* name() const noexcept override { return "getaddrinfo"; }
            std::string message(int code) const override { return gai_strerror(code); }
        };
        static ResolverCategory category;
        const auto code = error == EAI_NONAME ? ErrorCode::NotFound : error == EAI_MEMORY ? ErrorCode::ResourceExhausted : ErrorCode::SystemError;
        return std::unexpected(OsError{code, Operation::ResolveAddress, {error, category}});
#endif
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(raw, freeaddrinfo);
    std::vector<IpEndpoint> result;
    for (auto* entry = raw; entry; entry = entry->ai_next) {
        auto value = detail::decode_address(entry->ai_addr, static_cast<detail::AddressLength>(entry->ai_addrlen), Operation::ResolveAddress);
        if (!value || !std::holds_alternative<IpEndpoint>(*value)) continue;
        auto endpoint = std::get<IpEndpoint>(*value);
        if (std::find(result.begin(), result.end(), endpoint) == result.end()) result.push_back(endpoint);
    }
    if (result.empty()) return std::unexpected(OsError{ErrorCode::NotFound, Operation::ResolveAddress});
    return result;
}
} // namespace vinc::osal
