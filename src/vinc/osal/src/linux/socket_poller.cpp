// Linux socket readiness backend using epoll and an eventfd wake source.
// Return registration identities to the common layer, which filters stale watches.

#include <vinc/time.hpp>
#include <vinc/osal/detail/socket_native.hpp>
#include <sys/epoll.h>
#include <sys/eventfd.h>

namespace vinc::osal::detail {
struct NativeSocketPollerImpl {
    int queue = -1;
    int wake = -1;
    std::vector<epoll_event> events;

    ~NativeSocketPollerImpl() {
        if (wake >= 0) {
            ::close(wake);
        }

        if (queue >= 0) {
            ::close(queue);
        }
    }
};

NativeSocketPoller::NativeSocketPoller(std::unique_ptr<NativeSocketPollerImpl> impl)
    : impl_(std::move(impl)) {
}

NativeSocketPoller::~NativeSocketPoller() = default;

vinc::Result<std::unique_ptr<NativeSocketPoller>>
NativeSocketPoller::create(std::size_t maximum, vinc::Deadline, std::stop_token) {
    auto impl = std::make_unique<NativeSocketPollerImpl>();
    impl->events.resize(maximum + 1);
    impl->queue = epoll_create1(EPOLL_CLOEXEC);
    if (impl->queue < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    auto isolated = isolate_socket_handle(impl->queue, vinc::osal::ErrorOperation::InitializeService);
    if (!isolated) {
        return std::unexpected(isolated.error());
    }
    impl->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (impl->wake < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }
    isolated = isolate_socket_handle(impl->wake, vinc::osal::ErrorOperation::InitializeService);
    if (!isolated) {
        return std::unexpected(isolated.error());
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (epoll_ctl(impl->queue, EPOLL_CTL_ADD, impl->wake, &event) < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    return std::unique_ptr<NativeSocketPoller>(new NativeSocketPoller(std::move(impl)));
}

namespace {
vinc::Result<void> watch(int queue, int action, SocketHandle handle, std::uint64_t id,
                         SocketEvents interests) {
    epoll_event event{};
    event.data.u64 = id;
    event.events = EPOLLRDHUP;
    if (has_event(interests, SocketEvents::Readable)) {
        event.events |= EPOLLIN;
    }

    if (has_event(interests, SocketEvents::Writable)) {
        event.events |= EPOLLOUT;
    }

    if (epoll_ctl(queue, action, handle, &event) < 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::RegisterSocket));
    }

    return {};
}
}

vinc::Result<void> NativeSocketPoller::add(SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    return watch(impl_->queue, EPOLL_CTL_ADD, handle, id, interests);
}

vinc::Result<void> NativeSocketPoller::modify(SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    return watch(impl_->queue, EPOLL_CTL_MOD, handle, id, interests);
}

void NativeSocketPoller::remove(SocketHandle handle, std::uint64_t) noexcept {
    if (handle >= 0) {
        epoll_ctl(impl_->queue, EPOLL_CTL_DEL, handle, nullptr);
    }
}

vinc::Result<std::size_t> NativeSocketPoller::wait(std::span<NativeSocketEvent> output, int milliseconds) {
    // Asking only for the output capacity preserves epoll's round-robin fairness.
    const int ready =
        epoll_wait(impl_->queue, impl_->events.data(), static_cast<int>(output.size()), milliseconds);
    if (ready < 0) {
        if (errno == EINTR) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::WouldBlock, vinc::osal::ErrorOperation::WaitSocket});
        }

        return std::unexpected(socket_error(vinc::osal::ErrorOperation::WaitSocket));
    }

    if (!ready) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Timeout, vinc::osal::ErrorOperation::WaitSocket});
    }

    std::size_t count = 0;
    for (int i = 0; i < ready; ++i) {
        const auto& event = impl_->events[static_cast<std::size_t>(i)];
        if (!event.data.u64) {
            std::uint64_t value;
            ssize_t read;
            do {
                read = ::read(impl_->wake, &value, sizeof(value));
            } while (read < 0 && errno == EINTR);
            if (read < 0 && errno != EAGAIN) {
                return std::unexpected(socket_error(vinc::osal::ErrorOperation::WaitSocket));
            }
            continue;
        }

        auto flags = SocketEvents::None;
        if (event.events & EPOLLIN) {
            flags = flags | SocketEvents::Readable;
        }

        if (event.events & EPOLLOUT) {
            flags = flags | SocketEvents::Writable;
        }

        if (event.events & EPOLLERR) {
            flags = flags | SocketEvents::Error;
        }

        if (event.events & (EPOLLHUP | EPOLLRDHUP)) {
            flags = flags | SocketEvents::Hangup;
        }
        output[count++] = {event.data.u64, flags};
    }

    return count;
}

vinc::Result<void> NativeSocketPoller::wake() noexcept {
    const std::uint64_t value = 1;
    ssize_t written;
    do {
        written = ::write(impl_->wake, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);
    if (written < 0 && errno != EAGAIN) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::WakeSocket));
    }

    return {};
}
} // namespace vinc::osal::detail
