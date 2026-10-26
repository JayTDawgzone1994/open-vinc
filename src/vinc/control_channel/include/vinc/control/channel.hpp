#pragma once

// A connected Socket becomes a versioned session with a handshake and two-way heartbeat.
// This layer reports connection health; ModuleManager decides whether to stop or restart a module.

#include <vinc/time.hpp>
#include <vinc/control/protocol.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <chrono>
#include <memory>
#include <optional>

namespace vinc::control {
using Clock = std::chrono::steady_clock;
enum class Role : std::uint32_t {
    Central = 1,
    Module = 2
};
enum class State {
    Handshaking,
    Open,
    Closed
};

enum class ReceiveOverflow { CloseSession, DropNewest };
struct QueueStats {
    std::uint64_t dropped_frames = 0;
    std::uint64_t dropped_bytes = 0;
};
struct Options {
    // Local queue policies, not handshake fields. Defaults preserve reliable control behavior.
    ReceiveOverflow receive_overflow = ReceiveOverflow::CloseSession;
    std::size_t reserved_send_frames = 0;
    std::size_t reserved_send_bytes = 0;
    // Fixed for this session; peers must explicitly agree during Hello.
    // Disabling expiry retains heartbeat validation, bounded queues and transport failures.
    bool heartbeat_timeout_enabled = true;
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
    Result<void> wait_open(vinc::Deadline deadline);
    // Accepted into a bounded queue, not remotely acknowledged. Closed/Send keeps
    // a stored session fault as its cause; explicit close adds no redundant cause.
    Result<void> send(Frame frame, bool use_reserved_capacity = false);
    QueueStats queue_stats() const;
    // Drain already received frames before reporting a closed session.
    // A successful empty optional means there is no queued frame yet.
    Result<std::optional<Frame>> try_receive();
    Result<Frame> receive_until(vinc::Deadline deadline);
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
