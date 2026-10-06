#include <vinc/osal/detail/utf8.hpp>
#include <vinc/osal/detail/native_error.hpp>
// Convert portable IPv4, IPv6 and local endpoints to and from native addresses.
// Explicit hostname resolution is kept separate from nonblocking socket I/O.

#include <vinc/osal/detail/operation_error.hpp>
#include <vinc/osal/detail/socket_native.hpp>
#include <algorithm>
#include <charconv>
#include <mutex>
#include <new>

namespace vinc::osal::detail {

SocketRuntime::~SocketRuntime() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

vinc::Result<std::shared_ptr<SocketRuntime>> socket_runtime() {
    static std::mutex mutex;
    static std::weak_ptr<SocketRuntime> current;
    std::lock_guard lock(mutex);
    if (auto runtime = current.lock()) {
        return runtime;
    }
    // Allocate first; a failed WSAStartup must not trigger WSACleanup.
    auto* storage = ::operator new(sizeof(SocketRuntime));
#if defined(_WIN32)
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error) {
        ::operator delete(storage);
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService, error));
    }
#endif
    auto runtime = std::shared_ptr<SocketRuntime>(new (storage) SocketRuntime);
    current = runtime;
    return runtime;
}

vinc::Error socket_error(vinc::osal::ErrorOperation operation, int error) noexcept {
    vinc::osal::ErrorCode code = vinc::osal::ErrorCode::SystemError;
#if defined(_WIN32)
    switch (error) {
    case WSAEWOULDBLOCK:
        code = vinc::osal::ErrorCode::WouldBlock;
        break;
    case WSAEADDRINUSE:
        code = vinc::osal::ErrorCode::AddressInUse;
        break;
    case WSAEADDRNOTAVAIL:
        code = vinc::osal::ErrorCode::AddressNotAvailable;
        break;
    case WSAECONNREFUSED:
        code = vinc::osal::ErrorCode::ConnectionRefused;
        break;
    case WSAECONNRESET:
    case WSAECONNABORTED:
        code = vinc::osal::ErrorCode::ConnectionReset;
        break;
    case WSAENOTCONN:
    case WSAEDESTADDRREQ:
        code = vinc::osal::ErrorCode::NotConnected;
        break;
    case WSAESHUTDOWN:
        code = vinc::osal::ErrorCode::PeerClosed;
        break;
    case WSAEMSGSIZE:
        code = vinc::osal::ErrorCode::MessageTooLarge;
        break;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
        code = vinc::osal::ErrorCode::NetworkUnreachable;
        break;
    case WSAEAFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
    case WSAEOPNOTSUPP:
    case WSAENOPROTOOPT:
        code = vinc::osal::ErrorCode::Unsupported;
        break;
    case WSAEACCES:
        code = vinc::osal::ErrorCode::PermissionDenied;
        break;
    case WSAEINVAL:
    case WSAEFAULT:
        code = vinc::osal::ErrorCode::InvalidArgument;
        break;
    case WSAENOTSOCK:
        code = vinc::osal::ErrorCode::Closed;
        break;
    case WSAENOBUFS:
    case WSAEMFILE:
        code = vinc::osal::ErrorCode::ResourceExhausted;
        break;
    case WSAETIMEDOUT:
        code = vinc::osal::ErrorCode::Timeout;
        break;
    case WSAHOST_NOT_FOUND:
    case WSANO_DATA:
        code = vinc::osal::ErrorCode::NotFound;
        break;
    }

    return {code, operation, {vinc::NativeErrorSource::Winsock, static_cast<std::uint32_t>(error)}};
#else
    switch (error) {
    case EAGAIN:
        code = vinc::osal::ErrorCode::WouldBlock;
        break;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
        code = vinc::osal::ErrorCode::WouldBlock;
        break;
#endif
    case EADDRINUSE:
        code = vinc::osal::ErrorCode::AddressInUse;
        break;
    case EADDRNOTAVAIL:
        code = vinc::osal::ErrorCode::AddressNotAvailable;
        break;
    case ECONNREFUSED:
        code = vinc::osal::ErrorCode::ConnectionRefused;
        break;
    case ECONNRESET:
    case ECONNABORTED:
        code = vinc::osal::ErrorCode::ConnectionReset;
        break;
    case ENOTCONN:
    case EDESTADDRREQ:
        code = vinc::osal::ErrorCode::NotConnected;
        break;
    case EPIPE:
        code = vinc::osal::ErrorCode::PeerClosed;
        break;
    case EMSGSIZE:
        code = vinc::osal::ErrorCode::MessageTooLarge;
        break;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
        code = vinc::osal::ErrorCode::NetworkUnreachable;
        break;
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
    case EOPNOTSUPP:
    case ENOPROTOOPT:
        code = vinc::osal::ErrorCode::Unsupported;
        break;
    case EACCES:
    case EPERM:
        code = vinc::osal::ErrorCode::PermissionDenied;
        break;
    case EINVAL:
    case EFAULT:
    case ENAMETOOLONG:
        code = vinc::osal::ErrorCode::InvalidArgument;
        break;
    case EBADF:
    case ENOTSOCK:
        code = vinc::osal::ErrorCode::Closed;
        break;
    case ENOBUFS:
    case ENOMEM:
    case EMFILE:
    case ENFILE:
        code = vinc::osal::ErrorCode::ResourceExhausted;
        break;
    case ETIMEDOUT:
        code = vinc::osal::ErrorCode::Timeout;
        break;
    case ENOENT:
        code = vinc::osal::ErrorCode::NotFound;
        break;
    }

    return {code, operation, {detail::errno_source, error}};
#endif
}

int native_family(AddressFamily family) noexcept {
    switch (family) {
    case AddressFamily::IPv4:
        return AF_INET;
    case AddressFamily::IPv6:
        return AF_INET6;
    case AddressFamily::Local:
        return AF_UNIX;
    }

    return AF_UNSPEC;
}

int native_type(SocketType type) noexcept {
    return type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM;
}

vinc::Result<void> isolate_socket_handle(SocketHandle& handle, vinc::osal::ErrorOperation operation) {
#if !defined(_WIN32)
    if (handle >= 0 && handle < 3) {
        const int replacement = fcntl(handle, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) {
            return std::unexpected(socket_error(operation));
        }
        ::close(handle);
        handle = replacement;
    }
#else
    (void)handle;
    (void)operation;
#endif
    return {};
}

vinc::Result<void> configure_socket(SocketHandle& socket) {
    auto isolated = isolate_socket_handle(socket, vinc::osal::ErrorOperation::CreateSocket);
    if (!isolated) {
        return isolated;
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::CreateSocket));
    }

    // Windows socket handles are pointer-sized kernel tokens accepted by
    // SetHandleInformation. This conversion does not access a C++ object.
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0)) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::SystemError,
                                           vinc::osal::ErrorOperation::CreateSocket,
                                           {vinc::NativeErrorSource::Win32, GetLastError()}});
    }
#else
    const int flags = fcntl(socket, F_GETFL);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(socket, F_SETFD, FD_CLOEXEC) < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::CreateSocket));
    }
#if defined(VINC_OSAL_MACOS)
    const int one = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::CreateSocket));
    }
#endif
#endif
    return {};
}

vinc::Result<NativeAddress> encode_address(const SocketAddress& address,
                                           vinc::osal::ErrorOperation operation) {
    NativeAddress native;
    if (auto ip = std::get_if<IpEndpoint>(&address)) {
        if (auto v4 = std::get_if<IPv4Address>(&ip->address)) {
            sockaddr_in value{};
            value.sin_family = AF_INET;
            value.sin_port = htons(ip->port);
            std::memcpy(&value.sin_addr, v4->bytes.data(), 4);
            std::memcpy(&native.storage, &value, sizeof(value));
            native.size = sizeof(value);
        } else {
            const auto& v6 = std::get<IPv6Address>(ip->address);
            sockaddr_in6 value{};
            value.sin6_family = AF_INET6;
            value.sin6_port = htons(ip->port);
            value.sin6_scope_id = v6.scope_id;
            std::memcpy(&value.sin6_addr, v6.bytes.data(), 16);
            std::memcpy(&native.storage, &value, sizeof(value));
            native.size = sizeof(value);
        }
    } else {
        const auto& path = std::get<LocalEndpoint>(address).path;
        sockaddr_un value{};
        if (path.empty() || path.find('\0') != std::string::npos || path.size() >= sizeof(value.sun_path) ||
            !valid_utf8_encoding(path)) {
            return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation});
        }
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

vinc::Result<SocketAddress> decode_address(const sockaddr* address, AddressLength length,
                                           vinc::osal::ErrorOperation operation) {
    if (length < static_cast<AddressLength>(sizeof(address->sa_family))) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::SystemError, operation});
    }

    if (address->sa_family == AF_INET && length >= static_cast<AddressLength>(sizeof(sockaddr_in))) {
        // Native socket ABI boundary: the caller supplies sockaddr_storage
        // alignment; family and length above select a complete IPv4 layout.
        const auto* value = reinterpret_cast<const sockaddr_in*>(address);
        IPv4Address ip;
        std::memcpy(ip.bytes.data(), &value->sin_addr, 4);
        return SocketAddress{IpEndpoint{ip, ntohs(value->sin_port)}};
    }

    if (address->sa_family == AF_INET6 && length >= static_cast<AddressLength>(sizeof(sockaddr_in6))) {
        // The caller supplies aligned native storage; family and length above
        // select the complete IPv6 layout populated by the socket API.
        const auto* value = reinterpret_cast<const sockaddr_in6*>(address);
        IPv6Address ip;
        std::memcpy(ip.bytes.data(), &value->sin6_addr, 16);
        ip.scope_id = value->sin6_scope_id;
        return SocketAddress{IpEndpoint{ip, ntohs(value->sin6_port)}};
    }

    if (address->sa_family == AF_UNIX) {
        // The native family selects sockaddr_un in aligned sockaddr_storage.
        // Local addresses can be shorter than the full struct; count below limits
        // path access to the returned bytes and the native sun_path capacity.
        const auto* value = reinterpret_cast<const sockaddr_un*>(address);
        const auto count =
            length > static_cast<AddressLength>(offsetof(sockaddr_un, sun_path))
                ? std::min<std::size_t>(static_cast<std::size_t>(length) - offsetof(sockaddr_un, sun_path),
                                        sizeof(value->sun_path))
                : 0;
        const auto end = std::find(value->sun_path, value->sun_path + count, '\0');
        return SocketAddress{LocalEndpoint{std::string(value->sun_path, end)}};
    }

    return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Unsupported, operation});
}
} // namespace vinc::osal::detail

namespace vinc::osal {
vinc::Result<IPv4Address> IPv4Address::parse(std::string_view text) {
    if (text.empty() || text.size() >= INET_ADDRSTRLEN || text.find('\0') != std::string_view::npos) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ParseAddress});
    }
    char buffer[INET_ADDRSTRLEN]{};
    std::memcpy(buffer, text.data(), text.size());
    IPv4Address result;
    if (inet_pton(AF_INET, buffer, result.bytes.data()) != 1) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ParseAddress});
    }

    return result;
}

vinc::Result<IPv6Address> IPv6Address::parse(std::string_view text, std::uint32_t scope) {
    if (text.empty() || text.size() >= INET6_ADDRSTRLEN || text.find('\0') != std::string_view::npos) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ParseAddress});
    }
    char buffer[INET6_ADDRSTRLEN]{};
    std::memcpy(buffer, text.data(), text.size());
    IPv6Address result;
    result.scope_id = scope;
    if (inet_pton(AF_INET6, buffer, result.bytes.data()) != 1) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ParseAddress});
    }

    return result;
}

std::string IPv4Address::to_string() const {
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, bytes.data(), text, sizeof(text));
    return text;
}

std::string IPv6Address::to_string() const {
    char text[INET6_ADDRSTRLEN]{};
    inet_ntop(AF_INET6, bytes.data(), text, sizeof(text));
    return text;
}

AddressFamily address_family(const SocketAddress& value) noexcept {
    if (auto ip = std::get_if<IpEndpoint>(&value)) {
        return std::holds_alternative<IPv4Address>(ip->address) ? AddressFamily::IPv4 : AddressFamily::IPv6;
    }

    return AddressFamily::Local;
}

vinc::Result<std::vector<IpEndpoint>> resolve_host(std::string_view host, std::uint16_t port, SocketType type,
                                                   std::optional<AddressFamily> family) {
    if (host.empty() || host.find('\0') != std::string_view::npos ||
        (family && *family != AddressFamily::IPv4 && *family != AddressFamily::IPv6) ||
        (type != SocketType::Stream && type != SocketType::Datagram)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ResolveAddress});
    }

    auto runtime = detail::socket_runtime();
    if (!runtime) {
        return std::unexpected(detail::operation_error(ErrorOperation::ResolveAddress, runtime.error()));
    }
    addrinfo hints{};
    hints.ai_family = family ? detail::native_family(*family) : AF_UNSPEC;
    hints.ai_socktype = detail::native_type(type);
    hints.ai_flags = AI_NUMERICSERV;
    const std::string name(host);
    const std::string service = std::to_string(port);
    addrinfo* raw = nullptr;
    const int error = getaddrinfo(name.c_str(), service.c_str(), &hints, &raw);
    if (error) {
#if defined(_WIN32)
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ResolveAddress, error));
#else
        if (error == EAI_SYSTEM) {
            return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ResolveAddress));
        }

        const auto code = error == EAI_NONAME   ? vinc::osal::ErrorCode::NotFound
                          : error == EAI_MEMORY ? vinc::osal::ErrorCode::ResourceExhausted
                                                : vinc::osal::ErrorCode::SystemError;
        return std::unexpected(
            vinc::Error{code, vinc::osal::ErrorOperation::ResolveAddress, {detail::resolver_source, error}});
#endif
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(raw, freeaddrinfo);
    std::vector<IpEndpoint> result;
    for (auto* entry = raw; entry; entry = entry->ai_next) {
        auto value =
            detail::decode_address(entry->ai_addr, static_cast<detail::AddressLength>(entry->ai_addrlen),
                                   vinc::osal::ErrorOperation::ResolveAddress);
        if (!value || !std::holds_alternative<IpEndpoint>(*value)) {
            continue;
        }

        auto endpoint = std::get<IpEndpoint>(*value);
        if (std::find(result.begin(), result.end(), endpoint) == result.end()) {
            result.push_back(endpoint);
        }
    }

    if (result.empty()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::NotFound, vinc::osal::ErrorOperation::ResolveAddress});
    }

    return result;
}
} // namespace vinc::osal
