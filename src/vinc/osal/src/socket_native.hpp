#pragma once
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
inline int last_socket_error() noexcept { return WSAGetLastError(); }
inline int close_socket(SocketHandle value) noexcept { return closesocket(value); }
using PollFd = WSAPOLLFD;
inline int poll_sockets(PollFd* values, std::size_t count, int timeout) {
    return WSAPoll(values, static_cast<ULONG>(count), timeout);
}
#else
using SocketHandle = int;
using AddressLength = socklen_t;
inline constexpr SocketHandle invalid_socket = -1;
inline int last_socket_error() noexcept { return errno; }
inline int close_socket(SocketHandle value) noexcept { return ::close(value); }
using PollFd = pollfd;
inline int poll_sockets(PollFd* values, std::size_t count, int timeout) {
    return ::poll(values, count, timeout);
}
#endif
struct SocketRuntime { ~SocketRuntime(); };
Result<std::shared_ptr<SocketRuntime>> socket_runtime();
OsError socket_error(Operation, int) noexcept;
inline OsError socket_error(Operation operation) noexcept { return socket_error(operation, last_socket_error()); }
int native_family(AddressFamily) noexcept;
int native_type(SocketType) noexcept;
Result<void> isolate_socket_handle(SocketHandle&, Operation);
Result<void> configure_socket(SocketHandle&);
struct NativeAddress { sockaddr_storage storage{}; AddressLength size = 0; };
Result<NativeAddress> encode_address(const SocketAddress&, Operation);
Result<SocketAddress> decode_address(const sockaddr*, AddressLength, Operation);
enum class SocketPhase { Fresh, Bound, Listening, Connecting, Connected, Failed };
struct SocketState {
    std::shared_ptr<SocketRuntime> runtime;
    SocketHandle handle = invalid_socket;
    AddressFamily family = AddressFamily::IPv4;
    SocketType type = SocketType::Stream;
    SocketPhase phase = SocketPhase::Fresh;
    std::optional<OsError> connection_error;
    ~SocketState() { if (handle != invalid_socket) close_socket(handle); }
};
inline int set_native_option(SocketHandle socket, int level, int name, const void* value, AddressLength size) {
    return setsockopt(socket, level, name, reinterpret_cast<const char*>(value), size);
}
inline int get_native_option(SocketHandle socket, int level, int name, void* value, AddressLength* size) {
    return getsockopt(socket, level, name, reinterpret_cast<char*>(value), size);
}
struct NativeSocketEvent { std::uint64_t id; SocketEvents events; };
struct NativeSocketPollerImpl;
class NativeSocketPoller {
public:
    static Result<std::unique_ptr<NativeSocketPoller>> create(std::size_t, Deadline, std::stop_token);
    ~NativeSocketPoller();
    Result<void> add(SocketHandle, std::uint64_t, SocketEvents);
    Result<void> modify(SocketHandle, std::uint64_t, SocketEvents);
    void remove(SocketHandle, std::uint64_t) noexcept;
    Result<std::size_t> wait(std::span<NativeSocketEvent>, int milliseconds);
    Result<void> wake() noexcept;
private:
    explicit NativeSocketPoller(std::unique_ptr<NativeSocketPollerImpl>);
    std::unique_ptr<NativeSocketPollerImpl> impl_;
};
} // namespace vinc::osal::detail
