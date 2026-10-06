#pragma once

// Wait for readiness across multiple sockets without creating a thread per socket.
// A registration identifies one watch, so a reused OS handle cannot impersonate an old watch.

#include <vinc/osal/error.hpp>
#include <vinc/time.hpp>
#include <vinc/osal/socket.hpp>
#include <cstdint>

namespace vinc::osal {
enum class SocketEvents : std::uint8_t {
    None = 0,
    Readable = 1,
    Writable = 2,
    Error = 4,
    Hangup = 8
};

constexpr SocketEvents operator|(SocketEvents a, SocketEvents b) noexcept {
    return static_cast<SocketEvents>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool has_event(SocketEvents value, SocketEvents flag) noexcept {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

struct SocketRegistration {
    std::uint64_t value = 0;
    friend bool operator==(SocketRegistration, SocketRegistration) = default;
};

struct SocketReadyEvent {
    SocketRegistration registration;
    SocketEvents events = SocketEvents::None;
    std::uint64_t user_tag = 0;
};

namespace detail {
struct SocketPollerState;
}

class SocketPoller {
public:
    // Windows wake-pair setup is bounded by the earlier of this deadline and
    // five seconds. The token also cancels setup, not just subsequent waits.
    static vinc::Result<SocketPoller> create(std::size_t max_sockets = 1024,
                                             vinc::Deadline setup_deadline = (vinc::Deadline::max)(),
                                             std::stop_token = {});
    ~SocketPoller();
    SocketPoller(SocketPoller&&) noexcept;
    SocketPoller& operator=(SocketPoller&&) noexcept;
    SocketPoller(const SocketPoller&) = delete;
    SocketPoller& operator=(const SocketPoller&) = delete;
    vinc::Result<SocketRegistration> add(Socket&, SocketEvents interests, std::uint64_t user_tag = 0);
    vinc::Result<void> modify(SocketRegistration, SocketEvents interests);
    vinc::Result<void> remove(SocketRegistration);
    // Level-triggered readiness, not completed I/O. Zero means explicit wake;
    // timeout and stop are distinct errors. One waiter; no concurrent mutations.
    vinc::Result<std::size_t> wait_until(std::span<SocketReadyEvent>, vinc::Deadline, std::stop_token = {});
    vinc::Result<void> wake() noexcept; // cross-thread safe; lifetime must be synchronized
private:
    explicit SocketPoller(std::unique_ptr<detail::SocketPollerState>);
    std::unique_ptr<detail::SocketPollerState> state_;
};
} // namespace vinc::osal
