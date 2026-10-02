// Linux process wakeup backend: epoll observes pidfds and an eventfd wakes service work.
// The common observer still reaps children; unavailable native watches use its polling fallback.

#include "../posix/platform.hpp"

#include <cerrno>
#include <csignal>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace vinc::osal::detail {
namespace {
// Maximum native readiness events collected in one wait.
constexpr int max_events_per_wait = 32;
// Poll children that could not obtain a native exit watch.
constexpr int fallback_poll_interval_ms = 50;
}

struct MonitorImpl {
    int queue = -1;
    int wake = -1;

    ~MonitorImpl() {
        if (wake >= 0) {
            close(wake);
        }

        if (queue >= 0) {
            close(queue);
        }
    }
};

NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}

NativeMonitor::~NativeMonitor() = default;

Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    auto impl = std::make_unique<MonitorImpl>();
    impl->queue = epoll_create1(EPOLL_CLOEXEC);
    if (impl->queue < 0) {
        return std::unexpected(native_error(Operation::InitializeService, errno));
    }
    impl->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl->wake < 0) {
        return std::unexpected(native_error(Operation::InitializeService, errno));
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = impl->wake;
    if (epoll_ctl(impl->queue, EPOLL_CTL_ADD, impl->wake, &event) != 0) {
        return std::unexpected(native_error(Operation::InitializeService, errno));
    }

    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}

void NativeMonitor::wake() noexcept {
    const std::uint64_t one = 1;
    ssize_t result;
    do {
        result = write(impl_->wake, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    // EAGAIN means a wake is already pending.
}

Result<void> NativeMonitor::wait(bool polling) noexcept {
    epoll_event events[max_events_per_wait];
    const int count =
        epoll_wait(impl_->queue, events, max_events_per_wait, polling ? fallback_poll_interval_ms : -1);
    if (count < 0) {
        if (errno == EINTR) {
            return {};
        }

        return std::unexpected(native_error(Operation::Observe, errno));
    }

    for (int i = 0; i < count; ++i) {
        if (events[i].data.fd == impl_->wake) {
            std::uint64_t value;
            ssize_t result;
            do {
                result = read(impl_->wake, &value, sizeof(value));
            } while (result < 0 && errno == EINTR);
        }
    }

    return {};
}

Result<int> watch_process(MonitorImpl& monitor, pid_t pid) noexcept {
    const int fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (fd < 0) {
        return std::unexpected(native_error(Operation::Observe, errno));
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(monitor.queue, EPOLL_CTL_ADD, fd, &event) != 0) {
        const int error = errno;
        close(fd);
        return std::unexpected(native_error(Operation::Observe, error));
    }

    return fd;
}

void release_watch(int fd) noexcept {
    close(fd);
}

Result<TerminateResult> kill_process(pid_t pid, int watch) noexcept {
    const long result =
        watch >= 0 ? syscall(SYS_pidfd_send_signal, watch, SIGKILL, nullptr, 0) : kill(pid, SIGKILL);
    if (result == 0) {
        return TerminateResult::Requested;
    }

    if (errno == ESRCH) {
        return TerminateResult::AlreadyExited;
    }

    return std::unexpected(native_error(Operation::Terminate, errno));
}
} // namespace vinc::osal::detail
