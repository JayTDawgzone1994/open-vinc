#pragma once
#include <vinc/control/protocol.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <chrono>
#include <memory>
#include <optional>

namespace vinc::control {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
enum class Role : std::uint32_t { Central = 1, Module = 2 };
enum class State { Handshaking, Open, Closed };
enum class ErrorCode { Timeout, Closed, Protocol, Io, Backpressure, InvalidArgument };
struct Error { ErrorCode code; std::string message; std::optional<osal::OsError> os_error; };
template<class T> using Result = std::expected<T, Error>;
struct Options {
    Role role = Role::Central;
    std::uint64_t generation = 0;
    std::string token;
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
    std::chrono::milliseconds handshake_timeout{5000};
};
namespace detail { struct Peer; struct Engine; }
// A session never reconnects or replays requests. It owns the connected socket.
// Public operations are thread-safe; object destruction still needs caller synchronization.
class Session {
public:
    State state() const;
    std::optional<Error> error() const;
    Result<void> wait_open(Deadline);
    Result<void> send(Frame); // accepted into a bounded queue, not remotely acknowledged
    Result<std::optional<Frame>> try_receive();
    Result<Frame> receive_until(Deadline);
    void close();
private:
    friend class Service;
    explicit Session(std::shared_ptr<detail::Peer>);
    std::shared_ptr<detail::Peer> peer_;
};
// One I/O thread and one OSAL poller per service, shared by all its sessions.
// Application callbacks never execute on this thread.
class Service {
public:
    static Result<std::unique_ptr<Service>> create(std::size_t max_sessions = 256);
    ~Service();
    Result<std::shared_ptr<Session>> attach(osal::Socket, Options);
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
private:
    explicit Service(std::shared_ptr<detail::Engine>);
    std::shared_ptr<detail::Engine> engine_;
};
} // namespace vinc::control
