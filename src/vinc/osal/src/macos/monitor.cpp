// macOS process wakeup backend: kqueue NOTE_EXIT plus a user event for service work.
// Exit notifications do not reap children; the common observer still calls waitpid.

#include <vinc/osal/detail/posix/platform.hpp>
#include <vinc/osal/detail/posix/descriptor.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace vinc::osal::detail {
namespace {
// Maximum native readiness events collected in one wait.
constexpr int max_events_per_wait = 32;
// Poll children that could not obtain a native exit watch (50 ms).
constexpr long fallback_poll_interval_ns = 50'000'000;
}

struct MonitorImpl {
    int queue = -1;

    ~MonitorImpl() {
        if (queue >= 0) {
            close(queue);
        }
    }
};

NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}

NativeMonitor::~NativeMonitor() = default;

vinc::Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    auto impl = std::make_unique<MonitorImpl>();
    impl->queue = kqueue();
    if (impl->queue < 0) {
        return std::unexpected(native_error(vinc::osal::ErrorOperation::InitializeService, errno));
    }
    auto isolated = isolate_descriptor(impl->queue, ErrorOperation::InitializeService);
    if (!isolated) {
        return std::unexpected(isolated.error());
    }

    if (fcntl(impl->queue, F_SETFD, FD_CLOEXEC) < 0) {
        return std::unexpected(native_error(vinc::osal::ErrorOperation::InitializeService, errno));
    }
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(impl->queue, &event, 1, nullptr, 0, nullptr) < 0) {
        return std::unexpected(native_error(vinc::osal::ErrorOperation::InitializeService, errno));
    }

    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}

void NativeMonitor::wake() noexcept {
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result;
    do {
        result = kevent(impl_->queue, &event, 1, nullptr, 0, nullptr);
    } while (result < 0 && errno == EINTR);
}

vinc::Result<void> NativeMonitor::wait(bool polling) noexcept {
    struct kevent events[max_events_per_wait];
    const timespec timeout{0, fallback_poll_interval_ns};
    const int count =
        kevent(impl_->queue, nullptr, 0, events, max_events_per_wait, polling ? &timeout : nullptr);
    if (count < 0) {
        if (errno == EINTR) {
            return {};
        }

        return std::unexpected(native_error(vinc::osal::ErrorOperation::Observe, errno));
    }

    for (int i = 0; i < count; ++i) {
        if (events[i].flags & EV_ERROR) {
            return std::unexpected(
                native_error(vinc::osal::ErrorOperation::Observe, static_cast<int>(events[i].data)));
        }
    }

    return {};
}

vinc::Result<int> watch_process(MonitorImpl& monitor, pid_t pid) noexcept {
    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(monitor.queue, &event, 1, nullptr, 0, nullptr) < 0) {
        return std::unexpected(native_error(vinc::osal::ErrorOperation::Observe, errno));
    }

    return 0;
}

void release_watch(int) noexcept {
    // EV_ONESHOT removes the watch on exit. Never delete by PID after waitpid
    // has reaped it: that PID may already belong to a newly registered child.
}

vinc::Result<TerminateResult> kill_process(pid_t pid, int) noexcept {
    if (kill(pid, SIGKILL) == 0) {
        return TerminateResult::Requested;
    }

    if (errno == ESRCH) {
        return TerminateResult::AlreadyExited;
    }

    return std::unexpected(native_error(vinc::osal::ErrorOperation::Terminate, errno));
}
} // namespace vinc::osal::detail
