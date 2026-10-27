// Translate nonblocking OS socket calls into the shared OSAL contract.
// Deadline helpers wait for readiness and retry; they do not implement a message protocol.

#include <vinc/osal/detail/operation_error.hpp>
#include <vinc/time.hpp>
#include <vinc/osal/detail/socket_native.hpp>
#include <algorithm>

namespace vinc::osal {
namespace {
using namespace detail;

vinc::Result<void> valid(const std::shared_ptr<SocketState>& state, vinc::osal::ErrorOperation operation) {
    if (!state || state->handle == invalid_socket) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Closed, operation});
    }

    return {};
}

vinc::Result<void> datagram_family(const std::shared_ptr<SocketState>& state, AddressFamily family,
                                   vinc::osal::ErrorOperation operation) {
    auto check = valid(state, operation);
    if (!check) {
        return check;
    }

    if (state->type != SocketType::Datagram || state->family != family) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation});
    }

    return {};
}

bool interrupted(int error) noexcept {
#if defined(_WIN32)
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

vinc::Result<ReceiveResult> receive_native(SocketState& state, std::span<std::byte> buffer,
                                           NativeAddress* source) {
    if (buffer.size() > INT_MAX || (state.type == SocketType::Stream && buffer.empty())) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ReceiveSocket});
    }
    char dummy = 0;
    // char may alias the writable byte span's representation. The native
    // receive is synchronous, and its size is bounded by this span.
    auto* bytes = buffer.empty() ? &dummy : reinterpret_cast<char*>(buffer.data());
#if defined(_WIN32)
    WSABUF storage{static_cast<ULONG>(buffer.size()), bytes};
    DWORD transferred = 0;
    DWORD flags = 0;
    int result;
    do {
        if (source) {
            source->size = sizeof(source->storage);
            // sockaddr_storage supplies native address size and alignment;
            // WinSock fills it synchronously and returns the actual byte length.
            result =
                WSARecvFrom(state.handle, &storage, 1, &transferred, &flags,
                            reinterpret_cast<sockaddr*>(&source->storage), &source->size, nullptr, nullptr);
        } else {
            result = WSARecv(state.handle, &storage, 1, &transferred, &flags, nullptr, nullptr);
        }
    } while (result != 0 && interrupted(last_socket_error()));
    if (result != 0) {
        const int error = last_socket_error();
        if (error == WSAEMSGSIZE && state.type == SocketType::Datagram) {
            return ReceiveResult{buffer.size(), false, true};
        }

        return std::unexpected(socket_error(vinc::osal::ErrorOperation::ReceiveSocket, error));
    }

    return ReceiveResult{transferred, state.type == SocketType::Stream && transferred == 0, false};
#else
    // Darwin may omit the sender address for a zero-capacity recvmsg. Receive
    // into a scratch byte instead, consuming one datagram and reporting any
    // discarded payload as truncation without exposing bytes outside the span.
    iovec vector{bytes, buffer.empty() ? 1 : buffer.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (source) {
        message.msg_name = &source->storage;
        message.msg_namelen = sizeof(source->storage);
    }
    ssize_t transferred;
    do {
        transferred = recvmsg(state.handle, &message, 0);
    } while (transferred < 0 && errno == EINTR);
    if (transferred < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::ReceiveSocket));
    }

    if (source) {
        source->size = message.msg_namelen;
    }

    const auto received_bytes = static_cast<std::size_t>(transferred);
    return ReceiveResult{std::min(received_bytes, buffer.size()),
                         state.type == SocketType::Stream && transferred == 0,
                         received_bytes > buffer.size() || (message.msg_flags & MSG_TRUNC) != 0};
#endif
}

vinc::Result<std::size_t> send_native(SocketState& state, std::span<const std::byte> buffer,
                                      const NativeAddress* destination) {
    if (buffer.size() > INT_MAX) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::MessageTooLarge, vinc::osal::ErrorOperation::SendSocket});
    }

    const char dummy = 0;
    // char may read the span's object representation; send borrows it only
    // for this synchronous call and receives the checked byte count.
    const auto* data = buffer.empty() ? &dummy : reinterpret_cast<const char*>(buffer.data());
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    decltype(::send(state.handle, data, 0, flags)) transferred;
    // encode_address supplies an aligned sockaddr_storage plus the concrete
    // family's byte count. The OS interprets that layout through sockaddr*.
    do {
        transferred = destination ? ::sendto(state.handle, data, static_cast<int>(buffer.size()), flags,
                                             reinterpret_cast<const sockaddr*>(&destination->storage),
                                             destination->size)
                                  : ::send(state.handle, data, static_cast<int>(buffer.size()), flags);
    } while (transferred < 0 && interrupted(last_socket_error()));
    if (transferred < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::SendSocket));
    }

    return static_cast<std::size_t>(transferred);
}

struct OptionDescription {
    int level;
    int name;
    bool byte_value = false;
};

vinc::Result<OptionDescription> describe_option(const SocketState& state, SocketOptionKey key,
                                                vinc::osal::ErrorOperation operation) {
    const bool ip = state.family != AddressFamily::Local;
    const bool ipv6 = state.family == AddressFamily::IPv6;
    switch (key) {
    case SocketOptionKey::ReceiveBuffer:
        return OptionDescription{SOL_SOCKET, SO_RCVBUF};
    case SocketOptionKey::SendBuffer:
        return OptionDescription{SOL_SOCKET, SO_SNDBUF};
    case SocketOptionKey::NoDelay:
        if (ip && state.type == SocketType::Stream) {
            return OptionDescription{IPPROTO_TCP, TCP_NODELAY};
        }
        break;
    case SocketOptionKey::KeepAlive:
        if (ip && state.type == SocketType::Stream) {
            return OptionDescription{SOL_SOCKET, SO_KEEPALIVE};
        }
        break;
    case SocketOptionKey::IPv6Only:
        if (ipv6) {
            return OptionDescription{IPPROTO_IPV6, IPV6_V6ONLY};
        }
        break;
    case SocketOptionKey::Broadcast:
        if (state.family == AddressFamily::IPv4 && state.type == SocketType::Datagram) {
            return OptionDescription{SOL_SOCKET, SO_BROADCAST};
        }
        break;
    case SocketOptionKey::ReuseAddress:
#if !defined(_WIN32)
        if (ip) {
            return OptionDescription{SOL_SOCKET, SO_REUSEADDR};
        }
#endif
        break;
    case SocketOptionKey::ExclusiveAddressUse:
#if defined(_WIN32)
        if (ip) {
            return OptionDescription{SOL_SOCKET, SO_EXCLUSIVEADDRUSE};
        }
#endif
        break;
    case SocketOptionKey::MulticastLoop:
    case SocketOptionKey::MulticastHops:
        if (ip && state.type == SocketType::Datagram) {
            const bool loop = key == SocketOptionKey::MulticastLoop;
            if (ipv6) {
                return OptionDescription{IPPROTO_IPV6, loop ? IPV6_MULTICAST_LOOP : IPV6_MULTICAST_HOPS};
            }
#if defined(_WIN32)
            return OptionDescription{IPPROTO_IP, loop ? IP_MULTICAST_LOOP : IP_MULTICAST_TTL};
#else
            return OptionDescription{IPPROTO_IP, loop ? IP_MULTICAST_LOOP : IP_MULTICAST_TTL, true};
#endif
        }
        break;
    }

    return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Unsupported, operation});
}

template <class Action>
auto perform_until(Socket& socket, SocketEvents interest, vinc::Deadline deadline, std::stop_token token,
                   ErrorOperation operation, Action action) -> decltype(action()) {
    using Return = decltype(action());
    std::optional<SocketPoller> poller;
    for (;;) {
        if (token.stop_requested()) {
            return Return(std::unexpected(
                detail::operation_error(operation, vinc::Error{ErrorCode::Cancelled, ErrorOperation::WaitSocket})));
        }

        auto result = action();
        if (result) {
            return result;
        }

        if (!result.error().is(vinc::osal::ErrorCode::WouldBlock)) {
            return Return(std::unexpected(detail::operation_error(operation, result.error())));
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return Return(std::unexpected(
                detail::operation_error(operation, vinc::Error{ErrorCode::Timeout, ErrorOperation::WaitSocket})));
        }

        if (!poller) {
            auto created = SocketPoller::create(1, deadline, token);
            if (!created) {
                return Return(std::unexpected(detail::operation_error(operation, created.error())));
            }
            poller.emplace(std::move(*created));
            auto registration = poller->add(socket, interest);
            if (!registration) {
                return Return(std::unexpected(detail::operation_error(operation, registration.error())));
            }
        }
        SocketReadyEvent ready;
        auto waited = poller->wait_until({&ready, 1}, deadline, token);
        if (!waited) {
            return Return(std::unexpected(detail::operation_error(operation, waited.error())));
        }
    }
}
}

Socket::Socket(std::shared_ptr<detail::SocketState> state) : state_(std::move(state)) {
}

Socket::~Socket() = default;
Socket::Socket(Socket&&) noexcept = default;
Socket& Socket::operator=(Socket&&) noexcept = default;

Socket::operator bool() const noexcept {
    return state_ && state_->handle != detail::invalid_socket;
}

vinc::Result<Socket> Socket::open(AddressFamily family, SocketType type) {
    if ((family != AddressFamily::IPv4 && family != AddressFamily::IPv6 && family != AddressFamily::Local) ||
        (type != SocketType::Stream && type != SocketType::Datagram)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::CreateSocket});
    }
#if defined(_WIN32)
    if (family == AddressFamily::Local && type == SocketType::Datagram) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Unsupported, vinc::osal::ErrorOperation::CreateSocket});
    }
#endif
    auto runtime = detail::socket_runtime();
    if (!runtime) {
        return std::unexpected(detail::operation_error(ErrorOperation::CreateSocket, runtime.error()));
    }

    auto state = std::make_shared<detail::SocketState>();
    state->runtime = std::move(*runtime);
    state->family = family;
    state->type = type;
#if defined(_WIN32)
    state->handle = WSASocketW(detail::native_family(family), detail::native_type(type), 0, nullptr, 0,
                               WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(__linux__)
    state->handle =
        ::socket(detail::native_family(family), detail::native_type(type) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    state->handle = ::socket(detail::native_family(family), detail::native_type(type), 0);
#endif
    if (state->handle == detail::invalid_socket) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::CreateSocket));
    }

    auto configured = detail::configure_socket(state->handle);
    if (!configured) {
        return std::unexpected(configured.error());
    }

    if (family == AddressFamily::IPv6) {
        const int one = 1;
        if (detail::set_native_option(state->handle, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0) {
            return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::CreateSocket));
        }
    }

    return Socket(std::move(state));
}

vinc::Result<void> Socket::bind(const SocketAddress& address) {
    auto check = valid(state_, vinc::osal::ErrorOperation::BindSocket);
    if (!check) {
        return check;
    }

    if (state_->phase != detail::SocketPhase::Fresh) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::BindSocket});
    }

    if (address_family(address) != state_->family) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::BindSocket});
    }

    auto native = detail::encode_address(address, vinc::osal::ErrorOperation::BindSocket);
    if (!native) {
        return std::unexpected(native.error());
    }

    // encode_address prepared aligned native storage and the concrete layout's size.
    if (::bind(state_->handle, reinterpret_cast<const sockaddr*>(&native->storage), native->size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::BindSocket));
    }
    state_->phase = detail::SocketPhase::Bound;
    return {};
}

vinc::Result<void> Socket::listen(int backlog) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ListenSocket);
    if (!check) {
        return check;
    }

    if (backlog <= 0 || state_->type != SocketType::Stream ||
        (state_->phase != detail::SocketPhase::Bound && state_->phase != detail::SocketPhase::Listening)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ListenSocket});
    }

    if (::listen(state_->handle, backlog) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ListenSocket));
    }
    state_->phase = detail::SocketPhase::Listening;
    return {};
}

vinc::Result<Socket> Socket::try_accept() {
    auto check = valid(state_, vinc::osal::ErrorOperation::AcceptSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (state_->phase != detail::SocketPhase::Listening) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AcceptSocket});
    }

    auto child = std::make_shared<detail::SocketState>(); // no fallible allocation after accept
    child->runtime = state_->runtime;
    child->family = state_->family;
    child->type = state_->type;
    do {
#if defined(__linux__)
        child->handle = accept4(state_->handle, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        child->handle = ::accept(state_->handle, nullptr, nullptr);
#endif
    } while (child->handle == detail::invalid_socket && interrupted(detail::last_socket_error()));
    if (child->handle == detail::invalid_socket) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::AcceptSocket));
    }

    auto configured = detail::configure_socket(child->handle);
    if (!configured) {
        return std::unexpected(detail::operation_error(ErrorOperation::AcceptSocket, configured.error()));
    }
    child->phase = detail::SocketPhase::Connected;
    return Socket(std::move(child));
}

vinc::Result<ConnectProgress> Socket::try_connect(const SocketAddress& address) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ConnectSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (address_family(address) != state_->family) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ConnectSocket});
    }

    if (state_->phase == detail::SocketPhase::Connecting) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Busy, vinc::osal::ErrorOperation::ConnectSocket});
    }

    if (state_->phase == detail::SocketPhase::Failed) {
        return std::unexpected(*state_->connection_error);
    }

    if (state_->phase == detail::SocketPhase::Listening ||
        (state_->type == SocketType::Stream && state_->phase == detail::SocketPhase::Connected)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ConnectSocket});
    }

    auto native = detail::encode_address(address, vinc::osal::ErrorOperation::ConnectSocket);
    if (!native) {
        return std::unexpected(native.error());
    }

    // encode_address prepared aligned native storage and the concrete layout's size.
    const int result =
        ::connect(state_->handle, reinterpret_cast<const sockaddr*>(&native->storage), native->size);
    if (result == 0) {
        state_->phase = detail::SocketPhase::Connected;
        return ConnectProgress::Connected;
    }

    const int error = detail::last_socket_error();
#if defined(_WIN32)
    const bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
    const bool pending = error == EINPROGRESS || error == EALREADY || error == EINTR;
#endif
    if (pending) {
        state_->phase = detail::SocketPhase::Connecting;
        return ConnectProgress::InProgress;
    }

    auto failure = detail::socket_error(vinc::osal::ErrorOperation::ConnectSocket, error);
    // AF_UNIX EAGAIN means no attempt was queued (e.g. full listener backlog),
    // unlike EINPROGRESS. Never report a phantom pending connection.
    if (failure.is(vinc::osal::ErrorCode::WouldBlock)) {
        return std::unexpected(failure);
    }
    state_->phase = detail::SocketPhase::Failed;
    state_->connection_error = failure;
    return std::unexpected(failure);
}

vinc::Result<ConnectProgress> Socket::finish_connect() {
    auto check = valid(state_, vinc::osal::ErrorOperation::ConnectSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (state_->phase == detail::SocketPhase::Connected) {
        return ConnectProgress::Connected;
    }

    if (state_->phase == detail::SocketPhase::Failed) {
        return std::unexpected(*state_->connection_error);
    }

    if (state_->phase != detail::SocketPhase::Connecting) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::NotConnected, vinc::osal::ErrorOperation::ConnectSocket});
    }
    detail::PollFd descriptor{};
    descriptor.fd = state_->handle;
    descriptor.events = POLLOUT;
    int ready;
    do {
        ready = detail::poll_sockets(&descriptor, 1, 0);
    } while (ready < 0 && interrupted(detail::last_socket_error()));
    if (ready < 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ConnectSocket));
    }

    if (!ready) {
        return ConnectProgress::InProgress;
    }
    int error = 0;
    detail::AddressLength size = sizeof(error);
    if (detail::get_native_option(state_->handle, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ConnectSocket));
    }

    if (error) {
        state_->connection_error = detail::socket_error(vinc::osal::ErrorOperation::ConnectSocket, error);
        state_->phase = detail::SocketPhase::Failed;
        return std::unexpected(*state_->connection_error);
    }

    auto peer = peer_address();
    if (!peer) {
        if (peer.error().is(vinc::osal::ErrorCode::NotConnected)) {
            if (!(descriptor.revents & POLLHUP)) {
                return ConnectProgress::InProgress;
            }
            state_->phase = detail::SocketPhase::Failed;
            state_->connection_error = vinc::wrap_error(
                ErrorCode::PeerClosed, ErrorOperation::ConnectSocket, peer.error());
            return std::unexpected(*state_->connection_error);
        }

        return std::unexpected(detail::operation_error(ErrorOperation::ConnectSocket, peer.error()));
    }
    state_->phase = detail::SocketPhase::Connected;
    return ConnectProgress::Connected;
}

vinc::Result<std::size_t> Socket::try_send(std::span<const std::byte> buffer) {
    auto check = valid(state_, vinc::osal::ErrorOperation::SendSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    return send_native(*state_, buffer, nullptr);
}

vinc::Result<ReceiveResult> Socket::try_receive(std::span<std::byte> buffer) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ReceiveSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    return receive_native(*state_, buffer, nullptr);
}

vinc::Result<std::size_t> Socket::try_send_to(std::span<const std::byte> buffer,
                                              const SocketAddress& address) {
    auto check = valid(state_, vinc::osal::ErrorOperation::SendSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (state_->type != SocketType::Datagram || address_family(address) != state_->family) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::SendSocket});
    }

    auto native = detail::encode_address(address, vinc::osal::ErrorOperation::SendSocket);
    if (!native) {
        return std::unexpected(native.error());
    }

    auto sent = send_native(*state_, buffer, &*native);
    if (state_->phase == detail::SocketPhase::Fresh && state_->family != AddressFamily::Local) {
        // IP sendto may auto-bind even when transmission subsequently fails.
        // An unnamed local datagram sender, however, need not become bound.
        auto address_now = local_address();
        if (address_now && std::get<IpEndpoint>(*address_now).port != 0) {
            state_->phase = detail::SocketPhase::Bound;
        }
    }

    return sent;
}

vinc::Result<DatagramResult> Socket::try_receive_from(std::span<std::byte> buffer) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ReceiveSocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (state_->type != SocketType::Datagram) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ReceiveSocket});
    }
    detail::NativeAddress source;
    auto received = receive_native(*state_, buffer, &source);
    if (!received) {
        return std::unexpected(received.error());
    }
    // POSIX local datagram senders may be unnamed and supply zero address bytes.
    if (source.size == 0 && state_->family == AddressFamily::Local) {
        return DatagramResult{*received, LocalEndpoint{}};
    }

    // source is aligned sockaddr_storage filled by receive; decode uses its
    // native family and returned length while this local storage remains alive.
    auto decoded = detail::decode_address(reinterpret_cast<sockaddr*>(&source.storage), source.size,
                                          vinc::osal::ErrorOperation::ReceiveSocket);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    return DatagramResult{*received, std::move(*decoded)};
}

vinc::Result<Socket> Socket::accept_until(vinc::Deadline deadline, std::stop_token token) {
    return perform_until(*this, SocketEvents::Readable, deadline, token, ErrorOperation::AcceptSocket, [&] {
        return try_accept();
    });
}

vinc::Result<void> Socket::connect_until(const SocketAddress& address, vinc::Deadline deadline,
                                         std::stop_token token) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ConnectSocket);
    if (!check) {
        return check;
    }

    if (state_->phase == detail::SocketPhase::Listening || state_->phase == detail::SocketPhase::Connecting ||
        (state_->phase == detail::SocketPhase::Connected && state_->type == SocketType::Stream)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ConnectSocket});
    }

    if (token.stop_requested()) {
        auto ignored = close();
        (void)ignored;
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Cancelled, vinc::osal::ErrorOperation::ConnectSocket});
    }

    auto result = try_connect(address);
    if (result && *result == ConnectProgress::Connected) {
        return {};
    }

    if (!result) {
        const auto error = result.error();
        auto ignored = close();
        (void)ignored;
        return std::unexpected(error);
    }

    auto completed = perform_until(
        *this, SocketEvents::Writable, deadline, token, ErrorOperation::ConnectSocket,
        [&]() -> vinc::Result<void> {
            auto progress = finish_connect();
            if (!progress) {
                return std::unexpected(progress.error());
            }

            if (*progress == ConnectProgress::InProgress) {
                return std::unexpected(vinc::Error{vinc::osal::ErrorCode::WouldBlock,
                                                   vinc::osal::ErrorOperation::ConnectSocket});
            }

            return {};
        });
    if (!completed) {
        auto ignored = close();
        (void)ignored;
    }

    return completed;
}

vinc::Result<std::size_t> Socket::send_until(std::span<const std::byte> data, vinc::Deadline deadline,
                                             std::stop_token token) {
    return perform_until(*this, SocketEvents::Writable, deadline, token, ErrorOperation::SendSocket, [&] {
        return try_send(data);
    });
}

vinc::Result<ReceiveResult> Socket::receive_until(std::span<std::byte> data, vinc::Deadline deadline,
                                                  std::stop_token token) {
    return perform_until(*this, SocketEvents::Readable, deadline, token, ErrorOperation::ReceiveSocket, [&] {
        return try_receive(data);
    });
}

vinc::Result<std::size_t> Socket::send_to_until(std::span<const std::byte> data, const SocketAddress& target,
                                                vinc::Deadline deadline, std::stop_token token) {
    return perform_until(*this, SocketEvents::Writable, deadline, token, ErrorOperation::SendSocket, [&] {
        return try_send_to(data, target);
    });
}

vinc::Result<DatagramResult> Socket::receive_from_until(std::span<std::byte> data, vinc::Deadline deadline,
                                                        std::stop_token token) {
    return perform_until(*this, SocketEvents::Readable, deadline, token, ErrorOperation::ReceiveSocket, [&] {
        return try_receive_from(data);
    });
}

vinc::Result<void> Socket::shutdown(ShutdownDirection direction) {
    auto check = valid(state_, vinc::osal::ErrorOperation::ShutdownSocket);
    if (!check) {
        return check;
    }

    if (direction != ShutdownDirection::Read && direction != ShutdownDirection::Write &&
        direction != ShutdownDirection::Both) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::ShutdownSocket});
    }
    // SD_RECEIVE/SD_SEND/SD_BOTH and SHUT_RD/SHUT_WR/SHUT_RDWR use 0/1/2.
    int how = 2;
    if (direction == ShutdownDirection::Read) {
        how = 0;
    } else if (direction == ShutdownDirection::Write) {
        how = 1;
    }

    if (::shutdown(state_->handle, how) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::ShutdownSocket));
    }

    return {};
}

vinc::Result<void> Socket::close() {
    if (!*this) {
        return {};
    }

    const auto handle = std::exchange(state_->handle, detail::invalid_socket);
    const int result = detail::close_socket(handle);
    if (result != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::CloseSocket));
    }

    return {};
}

vinc::Result<SocketAddress> Socket::local_address() const {
    auto check = valid(state_, vinc::osal::ErrorOperation::QuerySocket);
    if (!check) {
        return std::unexpected(check.error());
    }
    detail::NativeAddress address;
    address.size = sizeof(address.storage);
    // sockaddr_storage provides native alignment and capacity. The following
    // decode reads the family-specific layout using the returned byte length.
    if (getsockname(state_->handle, reinterpret_cast<sockaddr*>(&address.storage), &address.size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::QuerySocket));
    }

    return detail::decode_address(reinterpret_cast<sockaddr*>(&address.storage), address.size,
                                  vinc::osal::ErrorOperation::QuerySocket);
}

vinc::Result<SocketAddress> Socket::peer_address() const {
    auto check = valid(state_, vinc::osal::ErrorOperation::QuerySocket);
    if (!check) {
        return std::unexpected(check.error());
    }
    detail::NativeAddress address;
    address.size = sizeof(address.storage);
    // sockaddr_storage provides native alignment and capacity. The following
    // decode reads the family-specific layout using the returned byte length.
    if (getpeername(state_->handle, reinterpret_cast<sockaddr*>(&address.storage), &address.size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::QuerySocket));
    }

    return detail::decode_address(reinterpret_cast<sockaddr*>(&address.storage), address.size,
                                  vinc::osal::ErrorOperation::QuerySocket);
}

vinc::Result<std::optional<vinc::Error>> Socket::take_pending_error() {
    auto check = valid(state_, vinc::osal::ErrorOperation::QuerySocket);
    if (!check) {
        return std::unexpected(check.error());
    }

    if (state_->phase == detail::SocketPhase::Connecting || state_->phase == detail::SocketPhase::Failed) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Busy, vinc::osal::ErrorOperation::QuerySocket});
    }
    int error = 0;
    detail::AddressLength size = sizeof(error);
    if (detail::get_native_option(state_->handle, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::QuerySocket));
    }

    if (error) {
        return std::optional<vinc::Error>{
            detail::socket_error(vinc::osal::ErrorOperation::QuerySocket, error)};
    }

    return std::optional<vinc::Error>{};
}

vinc::Result<void> Socket::set_option_value(detail::SocketOptionKey key, int value) {
    auto check = valid(state_, vinc::osal::ErrorOperation::SetSocketOption);
    if (!check) {
        return check;
    }

    auto option = describe_option(*state_, key, vinc::osal::ErrorOperation::SetSocketOption);
    if (!option) {
        return std::unexpected(option.error());
    }
    using Key = detail::SocketOptionKey;
    if (((key == Key::ReceiveBuffer || key == Key::SendBuffer) && value <= 0) ||
        (key == Key::MulticastHops && (value < 0 || value > 255))) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::SetSocketOption});
    }

    if ((key == Key::ReuseAddress || key == Key::ExclusiveAddressUse || key == Key::IPv6Only) &&
        state_->phase != detail::SocketPhase::Fresh) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::SetSocketOption});
    }

    const auto byte = static_cast<unsigned char>(value);
    if (detail::set_native_option(state_->handle, option->level, option->name,
                                  option->byte_value ? static_cast<const void*>(&byte) : &value,
                                  option->byte_value ? sizeof(byte) : sizeof(value)) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::SetSocketOption));
    }

    return {};
}

vinc::Result<int> Socket::get_option_value(detail::SocketOptionKey key) const {
    auto check = valid(state_, vinc::osal::ErrorOperation::GetSocketOption);
    if (!check) {
        return std::unexpected(check.error());
    }

    auto option = describe_option(*state_, key, vinc::osal::ErrorOperation::GetSocketOption);
    if (!option) {
        return std::unexpected(option.error());
    }
    int value = 0;
    unsigned char byte = 0;
    detail::AddressLength size = option->byte_value ? sizeof(byte) : sizeof(value);
    if (detail::get_native_option(state_->handle, option->level, option->name,
                                  option->byte_value ? static_cast<void*>(&byte) : &value, &size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::GetSocketOption));
    }

    return option->byte_value ? static_cast<int>(byte) : value;
}

vinc::Result<void> Socket::set_multicast_interface(IPv4Address address) {
    auto check = datagram_family(state_, AddressFamily::IPv4, vinc::osal::ErrorOperation::SetSocketOption);
    if (!check) {
        return check;
    }
    in_addr value{};
    std::memcpy(&value, address.bytes.data(), 4);
    if (detail::set_native_option(state_->handle, IPPROTO_IP, IP_MULTICAST_IF, &value, sizeof(value)) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::SetSocketOption));
    }

    return {};
}

vinc::Result<void> Socket::set_multicast_interface(std::uint32_t index) {
    auto check = datagram_family(state_, AddressFamily::IPv6, vinc::osal::ErrorOperation::SetSocketOption);
    if (!check) {
        return check;
    }

    if (detail::set_native_option(state_->handle, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof(index)) !=
        0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::SetSocketOption));
    }

    return {};
}

vinc::Result<IPv4Address> Socket::multicast_interface_v4() const {
    auto check = datagram_family(state_, AddressFamily::IPv4, vinc::osal::ErrorOperation::GetSocketOption);
    if (!check) {
        return std::unexpected(check.error());
    }
    IPv4Address value;
    detail::AddressLength size = 4;
    if (detail::get_native_option(state_->handle, IPPROTO_IP, IP_MULTICAST_IF, value.bytes.data(), &size) !=
        0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::GetSocketOption));
    }

    return value;
}

vinc::Result<std::uint32_t> Socket::multicast_interface_v6() const {
    auto check = datagram_family(state_, AddressFamily::IPv6, vinc::osal::ErrorOperation::GetSocketOption);
    if (!check) {
        return std::unexpected(check.error());
    }

    std::uint32_t value = 0;
    detail::AddressLength size = sizeof(value);
    if (detail::get_native_option(state_->handle, IPPROTO_IPV6, IPV6_MULTICAST_IF, &value, &size) != 0) {
        return std::unexpected(detail::socket_error(vinc::osal::ErrorOperation::GetSocketOption));
    }

    return value;
}

namespace {
vinc::Result<void> membership(const std::shared_ptr<SocketState>& state, IPv4Address group,
                              IPv4Address interface, bool join) {
    auto check = datagram_family(state, AddressFamily::IPv4, vinc::osal::ErrorOperation::SetSocketOption);
    if (!check) {
        return check;
    }

    if ((group.bytes[0] & 0xf0) != 0xe0) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::SetSocketOption});
    }
    ip_mreq value{};
    std::memcpy(&value.imr_multiaddr, group.bytes.data(), 4);
    std::memcpy(&value.imr_interface, interface.bytes.data(), 4);
    if (set_native_option(state->handle, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &value,
                          sizeof(value)) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::SetSocketOption));
    }

    return {};
}

vinc::Result<void> membership(const std::shared_ptr<SocketState>& state, IPv6Address group,
                              std::uint32_t index, bool join) {
    auto check = datagram_family(state, AddressFamily::IPv6, vinc::osal::ErrorOperation::SetSocketOption);
    if (!check) {
        return check;
    }

    if (group.bytes[0] != 0xff || (group.scope_id && group.scope_id != index)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::SetSocketOption});
    }
    ipv6_mreq value{};
    std::memcpy(&value.ipv6mr_multiaddr, group.bytes.data(), 16);
    value.ipv6mr_interface = index;
    if (set_native_option(state->handle, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &value,
                          sizeof(value)) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::SetSocketOption));
    }

    return {};
}
}

vinc::Result<void> Socket::join_multicast(IPv4Address group, IPv4Address interface) {
    return membership(state_, group, interface, true);
}

vinc::Result<void> Socket::leave_multicast(IPv4Address group, IPv4Address interface) {
    return membership(state_, group, interface, false);
}

vinc::Result<void> Socket::join_multicast(IPv6Address group, std::uint32_t index) {
    return membership(state_, group, index, true);
}

vinc::Result<void> Socket::leave_multicast(IPv6Address group, std::uint32_t index) {
    return membership(state_, group, index, false);
}
} // namespace vinc::osal
