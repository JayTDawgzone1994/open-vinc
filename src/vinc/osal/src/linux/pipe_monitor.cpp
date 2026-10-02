// Linux readiness and wakeup backend for the single Pipe I/O worker.
// epoll events trigger another nonblocking transfer attempt; they are not byte counts.

#include "../posix/pipe_platform.hpp"
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace vinc::osal::detail {
Result<std::unique_ptr<PipeMonitor>> PipeMonitor::create() {
    auto impl = std::make_unique<PipeMonitorImpl>();
    impl->queue = epoll_create1(EPOLL_CLOEXEC);
    if (impl->queue < 0) return std::unexpected(native_error(Operation::InitializeService, errno));
    auto isolated = isolate_pipe_descriptor(impl->queue, Operation::InitializeService);
    if (!isolated) return std::unexpected(isolated.error());
    impl->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl->wake < 0) return std::unexpected(native_error(Operation::InitializeService, errno));
    isolated = isolate_pipe_descriptor(impl->wake, Operation::InitializeService);
    if (!isolated) return std::unexpected(isolated.error());
    epoll_event event{}; event.events = EPOLLIN; event.data.fd = impl->wake;
    if (epoll_ctl(impl->queue, EPOLL_CTL_ADD, impl->wake, &event) < 0)
        return std::unexpected(native_error(Operation::InitializeService, errno));
    return std::unique_ptr<PipeMonitor>(new PipeMonitor(std::move(impl)));
}
void PipeMonitor::wake() noexcept {
    const std::uint64_t one = 1;
    ssize_t result;
    do { result = ::write(impl_->wake, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
}
Result<void> PipeMonitor::wait(int milliseconds) noexcept {
    epoll_event events[32];
    const int count = epoll_wait(impl_->queue, events, 32, milliseconds);
    if (count < 0) {
        if (errno == EINTR) return {};
        return std::unexpected(native_error(Operation::WaitIo, errno));
    }
    for (int i = 0; i < count; ++i) if (events[i].data.fd == impl_->wake) {
        std::uint64_t value;
        ssize_t result;
        do { result = ::read(impl_->wake, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    }
    return {};
}
Result<void> arm_pipe(PipeMonitorImpl& monitor, int fd, IoKind kind) noexcept {
    epoll_event event{}; event.events = kind == IoKind::Read ? EPOLLIN : EPOLLOUT; event.data.fd = fd;
    if (epoll_ctl(monitor.queue, EPOLL_CTL_ADD, fd, &event) < 0)
        return std::unexpected(native_error(kind == IoKind::Read ? Operation::ReadPipe : Operation::WritePipe, errno));
    return {};
}
void disarm_pipe(PipeMonitorImpl& monitor, int fd, IoKind) noexcept { epoll_ctl(monitor.queue, EPOLL_CTL_DEL, fd, nullptr); }
} // namespace vinc::osal::detail
