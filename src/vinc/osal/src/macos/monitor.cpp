#include <vinc/osal/detail/posix/resource_guard.hpp>
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
constexpr int max_events_per_wait = static_cast<int>(ProcessExitEvents::capacity);
// Retry unregistered watches and consumed notifications still awaiting reaping (50 ms).
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
    auto resources = acquire_resources(ErrorOperation::InitializeService);
    if (!resources) {
        return std::unexpected(resources.error());
    }

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
    struct kevent64_s event;
    EV_SET64(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0, 0, 0);
    if (kevent64(impl->queue, &event, 1, nullptr, 0, 0, nullptr) < 0) {
        return std::unexpected(native_error(vinc::osal::ErrorOperation::InitializeService, errno));
    }

    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}

void NativeMonitor::wake() noexcept {
    struct kevent64_s event;
    EV_SET64(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0);
    int result;
    do {
        result = kevent64(impl_->queue, &event, 1, nullptr, 0, 0, nullptr);
    } while (result < 0 && errno == EINTR);
}

vinc::Result<ProcessExitEvents> NativeMonitor::wait(bool polling) noexcept {
    struct kevent64_s events[max_events_per_wait];
    const timespec timeout{0, fallback_poll_interval_ns};
    const int count =
        kevent64(impl_->queue, nullptr, 0, events, max_events_per_wait, 0, polling ? &timeout : nullptr);
    if (count < 0) {
        if (errno == EINTR) {
            return {};
        }

        return std::unexpected(native_error(vinc::osal::ErrorOperation::Observe, errno));
    }

    ProcessExitEvents exits;
    for (int i = 0; i < count; ++i) {
        // On a mixed error batch the service retains monitor_error and polls
        // all owned children, including any consumed notifications not returned.
        if (events[i].flags & EV_ERROR) {
            return std::unexpected(
                native_error(vinc::osal::ErrorOperation::Observe, static_cast<int>(events[i].data)));
        }
        if (events[i].filter == EVFILT_PROC && (events[i].fflags & NOTE_EXIT)) {
            exits.ids[exits.count++] = ProcessId{events[i].udata};
        }
    }

    return exits;
}

vinc::Result<int> watch_process(MonitorImpl& monitor, pid_t pid, ProcessId id) noexcept {
    struct kevent64_s event;
    // Include the non-reused service identity in the kernel watch key. A new
    // registration for a reused PID must not retag an old queued notification.
    EV_SET64(&event, static_cast<std::uint64_t>(pid), EVFILT_PROC,
             EV_ADD | EV_ONESHOT | EV_UDATA_SPECIFIC, NOTE_EXIT, 0, id.value, 0, 0);
    if (kevent64(monitor.queue, &event, 1, nullptr, 0, 0, nullptr) < 0) {
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
