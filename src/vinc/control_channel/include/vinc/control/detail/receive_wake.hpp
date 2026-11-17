#pragma once

#include <vinc/control/error.hpp>
#include <vinc/time.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>

namespace vinc::control {
class Session;

namespace detail {
// Framework integration only. One consumer owns the scan/wait cycle; many
// Sessions and management producers may share this process-local notification.
// Construction follows the existing standard-library exception policy.
class ReceiveWake {
public:
    ReceiveWake() = default;
    ReceiveWake(const ReceiveWake&) = delete;
    ReceiveWake& operator=(const ReceiveWake&) = delete;

    // No frame storage, callbacks or per-notification allocation. Hints coalesce.
    void notify();
    // Call BEFORE scanning queues. Do not clear hints between scanning and wait.
    bool consume();
    // Does not consume the hint. Stop wins over pending; otherwise a pending
    // hint wins over deadline. Timeout/cancellation never clear pending work.
    // Keep a strong owner alive through this call and release all Peer/owner
    // locks before waiting. The caller joins its waiter before final destruction.
    Result<void> wait_until(Deadline deadline, std::stop_token stop = {});

private:
#ifdef VINC_CONTROL_RECEIVE_TESTING
    friend struct ReceiveWakeTestAccess;
#endif
    std::mutex mutex_;
    std::condition_variable changed_;
    bool pending_ = false;
};

struct ReceiveWakeAccess {
    // Replace the Session's weak association under its Peer mutex. Empty means
    // detach. Binding rechecks backlog/Open/closing/Closed, including rebinding
    // the same owner. Old owners may retain a harmless pending hint.
    static void bind(Session& session, const std::shared_ptr<ReceiveWake>& wake);
};
}
}
