#include "../posix/pipe_platform.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>

namespace vinc::osal::detail {
Result<std::unique_ptr<PipeMonitor>> PipeMonitor::create() {
    auto impl = std::make_unique<PipeMonitorImpl>();
    impl->queue = kqueue();
    if (impl->queue < 0) return std::unexpected(native_error(Operation::InitializeService, errno));
    auto isolated = isolate_pipe_descriptor(impl->queue, Operation::InitializeService);
    if (!isolated) return std::unexpected(isolated.error());
    if (fcntl(impl->queue, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(native_error(Operation::InitializeService, errno));
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(impl->queue, &event, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(native_error(Operation::InitializeService, errno));
    return std::unique_ptr<PipeMonitor>(new PipeMonitor(std::move(impl)));
}
void PipeMonitor::wake() noexcept {
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result;
    do { result = kevent(impl_->queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
}
Result<void> PipeMonitor::wait(int milliseconds) noexcept {
    struct kevent events[32];
    const timespec timeout{milliseconds < 0 ? 0 : milliseconds / 1000,
        milliseconds < 0 ? 0 : (milliseconds % 1000) * 1000000L};
    const int count = kevent(impl_->queue, nullptr, 0, events, 32, milliseconds < 0 ? nullptr : &timeout);
    if (count < 0) {
        if (errno == EINTR) return {};
        return std::unexpected(native_error(Operation::WaitIo, errno));
    }
    for (int i = 0; i < count; ++i)
        if ((events[i].flags & EV_ERROR) && events[i].data)
            return std::unexpected(native_error(Operation::WaitIo, static_cast<int>(events[i].data)));
    return {};
}
Result<void> arm_pipe(PipeMonitorImpl& monitor, int fd, IoKind kind) noexcept {
    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(fd), kind == IoKind::Read ? EVFILT_READ : EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    int result;
    do { result = kevent(monitor.queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
    if (result < 0) return std::unexpected(native_error(kind == IoKind::Read ? Operation::ReadPipe : Operation::WritePipe, errno));
    return {};
}
void disarm_pipe(PipeMonitorImpl& monitor, int fd, IoKind kind) noexcept {
    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(fd), kind == IoKind::Read ? EVFILT_READ : EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    int result;
    do { result = kevent(monitor.queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
}
} // namespace vinc::osal::detail
