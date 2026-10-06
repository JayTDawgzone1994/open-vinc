#pragma once

// Private socket handles, connection state and poller backend interface.
// Public code carries portable addresses and registration IDs instead of native descriptors.

#include <vinc/osal/error.hpp>
#include <vinc/time.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <climits>
#include <cstring>
#include <utility>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace vinc::osal::detail {
#if defined(_WIN32)
using SocketHandle = SOCKET;
using AddressLength = int;
inline constexpr SocketHandle invalid_socket = INVALID_SOCKET;

inline int last_socket_error() noexcept {
    return WSAGetLastError();
}

inline int close_socket(SocketHandle value) noexcept {
    return closesocket(value);
}

using PollFd = WSAPOLLFD;

inline int poll_sockets(PollFd* values, std::size_t count, int timeout) {
    return WSAPoll(values, static_cast<ULONG>(count), timeout);
}
#else
using SocketHandle = int;
using AddressLength = socklen_t;
inline constexpr SocketHandle invalid_socket = -1;

inline int last_socket_error() noexcept {
    return errno;
}

inline int close_socket(SocketHandle value) noexcept {
    return ::close(value);
}

using PollFd = pollfd;

inline int poll_sockets(PollFd* values, std::size_t count, int timeout) {
    return ::poll(values, count, timeout);
}
#endif
struct SocketRuntime {
    ~SocketRuntime();
};

vinc::Result<std::shared_ptr<SocketRuntime>> socket_runtime();
vinc::Error socket_error(vinc::osal::ErrorOperation, int) noexcept;

inline vinc::Error socket_error(vinc::osal::ErrorOperation operation) noexcept {
    return socket_error(operation, last_socket_error());
}

int native_family(AddressFamily) noexcept;
int native_type(SocketType) noexcept;
vinc::Result<void> isolate_socket_handle(SocketHandle&, vinc::osal::ErrorOperation);
vinc::Result<void> configure_socket(SocketHandle&);

struct NativeAddress {
    sockaddr_storage storage{};
    AddressLength size = 0;
};

vinc::Result<NativeAddress> encode_address(const SocketAddress&, vinc::osal::ErrorOperation);
vinc::Result<SocketAddress> decode_address(const sockaddr*, AddressLength, vinc::osal::ErrorOperation);
enum class SocketPhase {
    Fresh,
    Bound,
    Listening,
    Connecting,
    Connected,
    Failed
};

struct SocketState {
    std::shared_ptr<SocketRuntime> runtime;
    SocketHandle handle = invalid_socket;
    AddressFamily family = AddressFamily::IPv4;
    SocketType type = SocketType::Stream;
    SocketPhase phase = SocketPhase::Fresh;
    std::optional<vinc::Error> connection_error;

    ~SocketState() {
        if (handle != invalid_socket) {
            close_socket(handle);
        }
    }
};

inline int set_native_option(SocketHandle socket, int level, int name, const void* value,
                             AddressLength size) {
    // The native API reads size bytes of a live option object. char may alias
    // its representation; the caller supplies the option's native type and size.
    return setsockopt(socket, level, name, reinterpret_cast<const char*>(value), size);
}

inline int get_native_option(SocketHandle socket, int level, int name, void* value, AddressLength* size) {
    // The caller supplies a live native option object and its writable size.
    // char permits byte access; the API updates size to the bytes returned.
    return getsockopt(socket, level, name, reinterpret_cast<char*>(value), size);
}

struct NativeSocketEvent {
    std::uint64_t id;
    SocketEvents events;
};
struct NativeSocketPollerImpl;

class NativeSocketPoller {
public:
    static vinc::Result<std::unique_ptr<NativeSocketPoller>> create(std::size_t, vinc::Deadline,
                                                                    std::stop_token);
    ~NativeSocketPoller();
    vinc::Result<void> add(SocketHandle, std::uint64_t, SocketEvents);
    vinc::Result<void> modify(SocketHandle, std::uint64_t, SocketEvents);
    void remove(SocketHandle, std::uint64_t) noexcept;
    vinc::Result<std::size_t> wait(std::span<NativeSocketEvent>, int milliseconds);
    vinc::Result<void> wake() noexcept;

private:
    explicit NativeSocketPoller(std::unique_ptr<NativeSocketPollerImpl>);
    std::unique_ptr<NativeSocketPollerImpl> impl_;
};
} // namespace vinc::osal::detail
