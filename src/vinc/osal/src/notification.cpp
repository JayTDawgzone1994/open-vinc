#include <vinc/osal/detail/notification_state.hpp>
#include <vinc/osal/detail/operation_error.hpp>
#include <optional>
#include <utility>

namespace vinc::osal {
namespace {
constexpr std::size_t maximum_name_bytes = 24;

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_name_bytes) {
        return false;
    }

    for (const unsigned char character : name) {
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

struct WaitGuard {
    std::atomic_flag& waiting;
    ~WaitGuard() {
        waiting.clear();
    }
};
}

NotificationReceiver::NotificationReceiver() noexcept = default;
NotificationReceiver::~NotificationReceiver() = default;
NotificationReceiver::NotificationReceiver(NotificationReceiver&&) noexcept = default;
NotificationReceiver& NotificationReceiver::operator=(NotificationReceiver&&) noexcept = default;
NotificationReceiver::NotificationReceiver(std::unique_ptr<detail::NotificationState> state)
    : state_(std::move(state)) {
}
NotificationReceiver::operator bool() const noexcept {
    return state_ && state_->handle != -1;
}

Result<NotificationReceiver> NotificationReceiver::create(std::string_view name) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::CreateNotification});
    }

    auto state = std::make_unique<detail::NotificationState>();
    auto created = detail::create_notification(*state, name);
    if (!created) {
        return std::unexpected(created.error());
    }
    return NotificationReceiver(std::move(state));
}

Result<NotificationReceiver> NotificationReceiver::open(std::string_view name) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::OpenNotification});
    }

    auto state = std::make_unique<detail::NotificationState>();
    auto opened = detail::open_notification_receiver(*state, name);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    return NotificationReceiver(std::move(state));
}

Result<void> NotificationReceiver::wait_until(Deadline deadline, std::stop_token stop) {
    constexpr auto operation = ErrorOperation::WaitNotification;
    if (!*this) {
        return std::unexpected(Error{ErrorCode::Closed, operation});
    }
    if (state_->waiting.test_and_set()) {
        return std::unexpected(Error{ErrorCode::Busy, operation});
    }
    WaitGuard guard{state_->waiting};
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, operation});
    }

    Result<void> result;
    std::optional<Error> wake_error;
    {
        // The state remains alive through callback destruction. The callback
        // may run on the requesting thread; inspect its error only after joining
        // it through stop_callback's destructor. No framework thread is created.
        std::stop_callback cancel(stop, [&] {
            auto woke = detail::wake_notification(*state_);
            if (!woke) {
                wake_error = woke.error();
            }
        });
        result = detail::wait_notification(*state_, deadline, stop);
    }
    if (wake_error) {
        return std::unexpected(detail::operation_error(operation, *wake_error));
    }
    return result;
}

Result<void> NotificationReceiver::close() noexcept {
    if (!state_) {
        return {};
    }
    return state_->close_native();
}

Result<void> NotificationReceiver::unlink_name(std::string_view name) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::UnlinkNotification});
    }
    return detail::unlink_notification(name);
}

NotificationSender::NotificationSender() noexcept = default;
NotificationSender::~NotificationSender() = default;
NotificationSender::NotificationSender(NotificationSender&&) noexcept = default;
NotificationSender& NotificationSender::operator=(NotificationSender&&) noexcept = default;
NotificationSender::NotificationSender(std::unique_ptr<detail::NotificationState> state)
    : state_(std::move(state)) {
}
NotificationSender::operator bool() const noexcept {
    return state_ && state_->handle != -1;
}

Result<NotificationSender> NotificationSender::open(std::string_view name) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::OpenNotification});
    }
    auto state = std::make_unique<detail::NotificationState>();
    auto opened = detail::open_notification(*state, name);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    return NotificationSender(std::move(state));
}

Result<void> NotificationSender::notify() const noexcept {
    if (!*this) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Notify});
    }
    return detail::signal_notification(*state_);
}

Result<void> NotificationSender::close() noexcept {
    if (!state_) {
        return {};
    }
    return state_->close_native();
}
} // namespace vinc::osal
