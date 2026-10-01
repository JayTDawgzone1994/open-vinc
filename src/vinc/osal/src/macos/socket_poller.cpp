// macOS socket readiness backend using kqueue read/write filters and a user-event wakeup.
// The common poller owns registration lifetime and translates readiness for callers.

#include "../socket_native.hpp"
#include <sys/event.h>

namespace vinc::osal::detail {
struct NativeSocketPollerImpl {
    int queue = -1;
    std::vector<struct kevent> events;
    ~NativeSocketPollerImpl() { if (queue >= 0) ::close(queue); }
};
NativeSocketPoller::NativeSocketPoller(std::unique_ptr<NativeSocketPollerImpl> impl) : impl_(std::move(impl)) {}
NativeSocketPoller::~NativeSocketPoller() = default;
Result<std::unique_ptr<NativeSocketPoller>> NativeSocketPoller::create(std::size_t maximum, Deadline, std::stop_token) {
    static_assert(sizeof(std::uintptr_t) >= sizeof(std::uint64_t), "macOS backend requires 64-bit pointers");
    auto impl = std::make_unique<NativeSocketPollerImpl>(); impl->events.resize(maximum + 1);
    impl->queue = kqueue();
    if (impl->queue < 0) return std::unexpected(socket_error(Operation::InitializeService));
    auto isolated = isolate_socket_handle(impl->queue, Operation::InitializeService);
    if (!isolated) return std::unexpected(isolated.error());
    if (fcntl(impl->queue, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(socket_error(Operation::InitializeService));
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(impl->queue, &event, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(socket_error(Operation::InitializeService));
    return std::unique_ptr<NativeSocketPoller>(new NativeSocketPoller(std::move(impl)));
}
namespace {
Result<void> watch(int queue, SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    // Install both filters, enabling only requested directions. EV_RECEIPT
    // checks every change; caller removes both if registration fails.
    struct kevent changes[2], receipts[2];
    const auto data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    EV_SET(&changes[0], static_cast<uintptr_t>(handle), EVFILT_READ,
        EV_ADD | EV_RECEIPT | (has_event(interests, SocketEvents::Readable) ? EV_ENABLE : EV_DISABLE), 0, 0, data);
    EV_SET(&changes[1], static_cast<uintptr_t>(handle), EVFILT_WRITE,
        EV_ADD | EV_RECEIPT | (has_event(interests, SocketEvents::Writable) ? EV_ENABLE : EV_DISABLE), 0, 0, data);
    int result;
    do { result = kevent(queue, changes, 2, receipts, 2, nullptr); } while (result < 0 && errno == EINTR);
    if (result < 0) return std::unexpected(socket_error(Operation::RegisterSocket));
    if (result != 2) return std::unexpected(OsError{ErrorCode::SystemError, Operation::RegisterSocket});
    for (const auto& receipt : receipts) if (receipt.data)
        return std::unexpected(socket_error(Operation::RegisterSocket, static_cast<int>(receipt.data)));
    return {};
}
}
Result<void> NativeSocketPoller::add(SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    auto result = watch(impl_->queue, handle, id, interests);
    if (!result) remove(handle, id);
    return result;
}
Result<void> NativeSocketPoller::modify(SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    return watch(impl_->queue, handle, id, interests);
}
void NativeSocketPoller::remove(SocketHandle handle, std::uint64_t) noexcept {
    if (handle < 0) return;
    for (const auto filter : {EVFILT_READ, EVFILT_WRITE}) {
        struct kevent event;
        EV_SET(&event, static_cast<uintptr_t>(handle), filter, EV_DELETE, 0, 0, nullptr);
        int result;
        do { result = kevent(impl_->queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
    }
}
Result<std::size_t> NativeSocketPoller::wait(std::span<NativeSocketEvent> output, int milliseconds) {
    const timespec timeout{milliseconds < 0 ? 0 : milliseconds / 1000, milliseconds < 0 ? 0 : (milliseconds % 1000) * 1000000L};
    const int ready = kevent(impl_->queue, nullptr, 0, impl_->events.data(), static_cast<int>(output.size()), milliseconds < 0 ? nullptr : &timeout);
    if (ready < 0) {
        if (errno == EINTR) return std::unexpected(OsError{ErrorCode::WouldBlock, Operation::WaitSocket});
        return std::unexpected(socket_error(Operation::WaitSocket));
    }
    if (!ready) return std::unexpected(OsError{ErrorCode::Timeout, Operation::WaitSocket});
    std::size_t count = 0;
    for (int i = 0; i < ready; ++i) {
        const auto& event = impl_->events[static_cast<std::size_t>(i)];
        if (event.filter == EVFILT_USER) continue;
        auto flags = event.filter == EVFILT_READ ? SocketEvents::Readable : SocketEvents::Writable;
        if (event.flags & EV_EOF) flags = flags | SocketEvents::Hangup;
        if ((event.flags & EV_ERROR) || ((event.flags & EV_EOF) && event.fflags)) flags = flags | SocketEvents::Error;
        output[count++] = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event.udata)), flags};
    }
    return count;
}
Result<void> NativeSocketPoller::wake() noexcept {
    struct kevent event; EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result;
    do { result = kevent(impl_->queue, &event, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
    if (result < 0) return std::unexpected(socket_error(Operation::WakeSocket));
    return {};
}
} // namespace vinc::osal::detail
