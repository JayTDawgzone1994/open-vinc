#pragma once

#include <vinc/osal/error.hpp>
#include <vinc/time.hpp>
#include <memory>
#include <stop_token>
#include <string_view>

namespace vinc::osal {
namespace detail {
struct NotificationState;
}

// One receiver, many senders, same host/user namespace. Notifications are hints:
// they may coalesce and carry neither payload nor an exact occurrence count.
class NotificationReceiver {
public:
    NotificationReceiver() noexcept;
    ~NotificationReceiver();
    NotificationReceiver(NotificationReceiver&&) noexcept;
    NotificationReceiver& operator=(NotificationReceiver&&) noexcept;
    NotificationReceiver(const NotificationReceiver&) = delete;
    NotificationReceiver& operator=(const NotificationReceiver&) = delete;
    explicit operator bool() const noexcept;

    // Exclusive creation. Publish the name only after success; never reuse an
    // active/retired generation's name. Names: 1..24 ASCII letters/digits/_/-.
    [[nodiscard]] static Result<NotificationReceiver> create(std::string_view name);
    // Open an existing endpoint without creating/resetting it. Handles may
    // overlap during handoff, but callers must keep one active waiter across
    // all handles/processes. This does not claim exclusive receiver ownership.
    [[nodiscard]] static Result<NotificationReceiver> open(std::string_view name);
    // A second waiter on THIS object returns Busy; separately opened handles
    // are not globally coordinated. Cancellation takes precedence
    // when observed, and can consume/leave a hint. Check actual work after wake.
    // An already pending hint can succeed even at an expired deadline.
    [[nodiscard]] Result<void> wait_until(Deadline, std::stop_token = {});
    // Synchronize close/move/destruction with all operations. Close is NOT cancel.
    [[nodiscard]] Result<void> close() noexcept;
    // POSIX removes only the name; live endpoints survive. Windows: Unsupported.
    // Designated owner only; absent names return NotFound. Destructor never unlinks.
    [[nodiscard]] static Result<void> unlink_name(std::string_view name);

private:
    explicit NotificationReceiver(std::unique_ptr<detail::NotificationState>);
    std::unique_ptr<detail::NotificationState> state_;
};

class NotificationSender {
public:
    NotificationSender() noexcept;
    ~NotificationSender();
    NotificationSender(NotificationSender&&) noexcept;
    NotificationSender& operator=(NotificationSender&&) noexcept;
    NotificationSender(const NotificationSender&) = delete;
    NotificationSender& operator=(const NotificationSender&) = delete;
    explicit operator bool() const noexcept;

    [[nodiscard]] static Result<NotificationSender> open(std::string_view name);
    // Concurrent notify calls are supported. Does not wait for a receiver to
    // consume work. Success is not acknowledgement or proof of peer liveness.
    [[nodiscard]] Result<void> notify() const noexcept;
    [[nodiscard]] Result<void> close() noexcept;

private:
    explicit NotificationSender(std::unique_ptr<detail::NotificationState>);
    std::unique_ptr<detail::NotificationState> state_;
};
} // namespace vinc::osal
