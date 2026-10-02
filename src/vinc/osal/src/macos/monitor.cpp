// macOS process wakeup backend: kqueue NOTE_EXIT plus a user event for service work.
// Exit notifications do not reap children; the common observer still calls waitpid.

#include "../posix/platform.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace vinc::osal::detail {
struct MonitorImpl {
    int queue = -1;
    ~MonitorImpl() { if (queue >= 0) close(queue); }
};
NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {}
NativeMonitor::~NativeMonitor() = default;
Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    auto impl = std::make_unique<MonitorImpl>();
    impl->queue = kqueue();
    if (impl->queue < 0) return std::unexpected(native_error(Operation::InitializeService, errno));
    if (fcntl(impl->queue, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(native_error(Operation::InitializeService, errno));
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(impl->queue, &event, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(native_error(Operation::InitializeService, errno));
    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}
void NativeMonitor::wake() noexcept {
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result;
    do { result = kevent(impl_->queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
}
Result<void> NativeMonitor::wait(bool polling) noexcept {
    struct kevent events[32];
    const timespec timeout{0, 50'000'000};
    const int count = kevent(impl_->queue, nullptr, 0, events, 32, polling ? &timeout : nullptr);
    if (count < 0) {
        if (errno == EINTR) return {};
        return std::unexpected(native_error(Operation::Observe, errno));
    }
    for (int i = 0; i < count; ++i)
        if (events[i].flags & EV_ERROR)
            return std::unexpected(native_error(Operation::Observe, static_cast<int>(events[i].data)));
    return {};
}
Result<int> watch_process(MonitorImpl& monitor, pid_t pid) noexcept {
    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(monitor.queue, &event, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(native_error(Operation::Observe, errno));
    return 0;
}
void release_watch(int) noexcept {
    // EV_ONESHOT removes the watch on exit. Never delete by PID after waitpid
    // has reaped it: that PID may already belong to a newly registered child.
}
Result<TerminateResult> kill_process(pid_t pid, int) noexcept {
    if (kill(pid, SIGKILL) == 0) return TerminateResult::Requested;
    if (errno == ESRCH) return TerminateResult::AlreadyExited;
    return std::unexpected(native_error(Operation::Terminate, errno));
}
} // namespace vinc::osal::detail
