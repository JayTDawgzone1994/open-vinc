#pragma once

#include <vinc/osal/notification.hpp>
#include <atomic>
#include <cstdint>

namespace vinc::osal::detail {
struct NotificationState {
    std::intptr_t handle = -1;
    // POSIX receiver owns a separate writer for cancellation and to prevent EOF
    // while there are no external senders. Windows uses the event handle itself.
    std::intptr_t writer = -1;
    std::atomic_flag waiting = ATOMIC_FLAG_INIT;
    ~NotificationState();
    Result<void> close_native() noexcept;
};

Result<void> create_notification(NotificationState&, std::string_view);
Result<void> open_notification_receiver(NotificationState&, std::string_view);
Result<void> open_notification(NotificationState&, std::string_view);
Result<void> unlink_notification(std::string_view);
Result<void> signal_notification(const NotificationState&) noexcept;
Result<void> wake_notification(const NotificationState&) noexcept;
Result<void> wait_notification(NotificationState&, Deadline, std::stop_token) noexcept;
} // namespace vinc::osal::detail
