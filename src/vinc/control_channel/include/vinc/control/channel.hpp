#pragma once

// A connected Socket becomes a versioned session with a handshake and two-way heartbeat.
// This layer reports connection health; ModuleManager decides whether to stop or restart a module.

#include <vinc/control/protocol.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <chrono>
#include <memory>
#include <optional>

namespace vinc::control {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
enum class Role : std::uint32_t {
    Central = 1,
    Module = 2
};
enum class State {
    Handshaking,
    Open,
    Closed
};
enum class ErrorCode {
    Timeout,
    Closed,
    Protocol,
    Io,
    Backpressure,
    InvalidArgument
};

struct Error {
    ErrorCode code;
    std::string message;
    std::optional<osal::OsError> os_error;
};
template <class T> using Result = std::expected<T, Error>;

struct Options {
    Role role = Role::Central;
    std::uint64_t generation = 0;
    std::string token;
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
    std::chrono::milliseconds handshake_timeout{5000};
};

namespace detail {
struct Peer;
struct Engine;
constexpr std::size_t default_session_capacity = 256;
}

// A session never reconnects or replays requests. It owns the connected socket.
// Public operations are thread-safe; object destruction still needs caller synchronization.
class Session {
public:
    State state() const;
    std::optional<Error> error() const;
    // A waiting timeout does not itself close the session or cancel its handshake.
    Result<void> wait_open(Deadline deadline);
    Result<void> send(Frame frame); // accepted into a bounded queue, not remotely acknowledged
    // Drain already received frames before reporting a closed session.
    // A successful empty optional means there is no queued frame yet.
    Result<std::optional<Frame>> try_receive();
    Result<Frame> receive_until(Deadline deadline);
    // Request asynchronous socket cleanup by the I/O worker.
    void close();

private:
    friend class Service;
    explicit Session(std::shared_ptr<detail::Peer> peer);
    std::shared_ptr<detail::Peer> peer_;
};

// One I/O thread and one OSAL poller per service, shared by all its sessions.
// Application callbacks never execute on this thread.
class Service {
public:
    static Result<std::unique_ptr<Service>>
    create(std::size_t max_sessions = detail::default_session_capacity);
    ~Service();
    // Consumes the socket even on failure. The worker sees it only after preparation
    // and capacity checks succeed; Session may outlive Service but cannot reconnect.
    Result<std::shared_ptr<Session>> attach(osal::Socket socket, Options options);
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

private:
    explicit Service(std::shared_ptr<detail::Engine> engine);
    std::shared_ptr<detail::Engine> engine_;
};
} // namespace vinc::control
