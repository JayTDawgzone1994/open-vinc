// Windows socket readiness backend using WSAPoll.
// A private connected loopback pair wakes the same wait for cancellation or cross-thread work.

#include <vinc/time.hpp>
#include <vinc/osal/detail/socket_native.hpp>
#include <algorithm>
#include <mutex>

namespace vinc::osal::detail {
struct NativeSocketPollerImpl {
    struct Entry {
        SocketHandle handle;
        std::uint64_t id;
        short interests;
    };

    std::shared_ptr<SocketRuntime> runtime;
    SocketHandle reader = invalid_socket;
    SocketHandle writer = invalid_socket;
    std::vector<Entry> entries;
    std::vector<PollFd> descriptors;
    std::mutex wake_mutex;
    bool wake_pending = false;
    std::size_t cursor = 0;

    ~NativeSocketPollerImpl() {
        if (reader != invalid_socket) {
            close_socket(reader);
        }

        if (writer != invalid_socket) {
            close_socket(writer);
        }
    }
};

NativeSocketPoller::NativeSocketPoller(std::unique_ptr<NativeSocketPollerImpl> impl)
    : impl_(std::move(impl)) {
}

NativeSocketPoller::~NativeSocketPoller() = default;

namespace {
// Setup has no wake socket yet, so cancellable waits use a short polling interval.
constexpr auto maximum_wake_setup_time = std::chrono::seconds(5);
constexpr int setup_cancel_poll_interval_ms = 10;
constexpr int wake_listener_backlog = 1;

// Owners are supplied by create(); partial success is cleaned up there.
vinc::Result<void> open_wake_sockets(SocketState& listener, NativeSocketPollerImpl& impl) {
    listener.handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    impl.writer = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (listener.handle == invalid_socket || impl.writer == invalid_socket) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    for (auto* handle : {&listener.handle, &impl.writer}) {
        auto configured = configure_socket(*handle);
        if (!configured) {
            return std::unexpected(configured.error());
        }
    }

    return {};
}

vinc::Result<void> begin_wake_connection(SocketState& listener, SocketHandle writer) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int enabled = 1;
    // These IPv4 calls use a live, aligned sockaddr_in. WinSock's sockaddr*
    // is a generic API view; sizeof(address) bounds access to the concrete layout.
    if (set_native_option(listener.handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &enabled, sizeof(enabled)) != 0 ||
        ::bind(listener.handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.handle, wake_listener_backlog) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    int size = sizeof(address);
    if (getsockname(listener.handle, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    // getsockname returned this IPv4 endpoint and its native byte length.
    const int connected = ::connect(writer, reinterpret_cast<sockaddr*>(&address), size);
    if (connected != 0 && last_socket_error() != WSAEWOULDBLOCK) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    return {};
}

vinc::Result<void> wait_for_setup(SocketHandle handle, short interests, vinc::Deadline deadline,
                                  std::stop_token token) {
    for (;;) {
        if (token.stop_requested()) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::Cancelled, vinc::osal::ErrorOperation::InitializeService});
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::Timeout, vinc::osal::ErrorOperation::InitializeService});
        }

        PollFd descriptor{handle, interests, 0};
        int timeout = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
        // Only during wake-pair construction, before the wake primitive
        // exists. Normal waits use an event-driven stop callback.
        if (token.stop_possible()) {
            timeout = std::min(timeout, setup_cancel_poll_interval_ms);
        }
        int ready = poll_sockets(&descriptor, 1, timeout);
        if (ready > 0) {
            return {};
        }

        if (ready == 0) {
            continue;
        }

        if (last_socket_error() != WSAEINTR) {
            return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
        }
    }
}

vinc::Result<sockaddr_in> finish_wake_connection(SocketHandle writer, vinc::Deadline deadline,
                                                 std::stop_token token) {
    auto writable = wait_for_setup(writer, POLLWRNORM, deadline, token);
    if (!writable) {
        return std::unexpected(writable.error());
    }

    int error = 0;
    int size = sizeof(error);
    if (get_native_option(writer, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    if (error) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService, error));
    }

    sockaddr_in own_address{};
    size = sizeof(own_address);
    // The IPv4 writer returns sockaddr_in into this aligned object; size is its capacity.
    if (getsockname(writer, reinterpret_cast<sockaddr*>(&own_address), &size) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    return own_address;
}

// Each accepted handle is immediately owned by impl. Unrelated connections are
// closed here; the matching reader stays owned by impl even if later setup fails.
vinc::Result<void> accept_wake_reader(SocketHandle listener, NativeSocketPollerImpl& impl,
                                      const sockaddr_in& own_address, vinc::Deadline deadline,
                                      std::stop_token token) {
    for (;;) {
        auto readable = wait_for_setup(listener, POLLRDNORM, deadline, token);
        if (!readable) {
            return std::unexpected(readable.error());
        }

        sockaddr_in peer{};
        int size = sizeof(peer);
        // The listener is IPv4; peer is a live sockaddr_in with its full capacity supplied.
        impl.reader = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &size);
        if (impl.reader == invalid_socket) {
            if (last_socket_error() == WSAEWOULDBLOCK || last_socket_error() == WSAEINTR) {
                continue;
            }

            return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
        }

        if (peer.sin_port == own_address.sin_port && peer.sin_addr.s_addr == own_address.sin_addr.s_addr) {
            break;
        }
        close_socket(impl.reader);
        impl.reader = invalid_socket;
    }

    return {};
}

vinc::Result<void> configure_wake_pair(NativeSocketPollerImpl& impl) {
    const int enabled = 1;
    auto configured = configure_socket(impl.reader);
    if (!configured) {
        return std::unexpected(configured.error());
    }

    if (set_native_option(impl.writer, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::InitializeService));
    }

    return {};
}
} // namespace

vinc::Result<std::unique_ptr<NativeSocketPoller>>
NativeSocketPoller::create(std::size_t maximum, vinc::Deadline setup_deadline, std::stop_token token) {
    auto runtime = socket_runtime();
    if (!runtime) {
        return std::unexpected(runtime.error());
    }

    auto impl = std::make_unique<NativeSocketPollerImpl>();
    impl->runtime = std::move(*runtime);
    impl->entries.reserve(maximum);
    impl->descriptors.resize(maximum + 1);

    // A connected loopback TCP pair is a reliable readiness wake primitive.
    // Keep the temporary listener alive until the final poller is constructed.
    SocketState listener;
    listener.runtime = impl->runtime;
    auto opened = open_wake_sockets(listener, *impl);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto connecting = begin_wake_connection(listener, impl->writer);
    if (!connecting) {
        return std::unexpected(connecting.error());
    }

    const auto deadline =
        std::min(setup_deadline, std::chrono::steady_clock::now() + maximum_wake_setup_time);
    auto own_address = finish_wake_connection(impl->writer, deadline, token);
    if (!own_address) {
        return std::unexpected(own_address.error());
    }

    auto accepted = accept_wake_reader(listener.handle, *impl, *own_address, deadline, token);
    if (!accepted) {
        return std::unexpected(accepted.error());
    }

    auto configured = configure_wake_pair(*impl);
    if (!configured) {
        return std::unexpected(configured.error());
    }

    return std::unique_ptr<NativeSocketPoller>(new NativeSocketPoller(std::move(impl)));
}

namespace {
short native_interests(SocketEvents value) {
    return static_cast<short>((has_event(value, SocketEvents::Readable) ? POLLRDNORM : 0) |
                              (has_event(value, SocketEvents::Writable) ? POLLWRNORM : 0));
}
}

vinc::Result<void> NativeSocketPoller::add(SocketHandle handle, std::uint64_t id, SocketEvents interests) {
    impl_->entries.push_back({handle, id, native_interests(interests)});
    return {};
}

vinc::Result<void> NativeSocketPoller::modify(SocketHandle, std::uint64_t id, SocketEvents interests) {
    for (auto& entry : impl_->entries) {
        if (entry.id == id) {
            entry.interests = native_interests(interests);
            return {};
        }
    }

    return std::unexpected(
        vinc::Error{vinc::osal::ErrorCode::NotFound, vinc::osal::ErrorOperation::RegisterSocket});
}

void NativeSocketPoller::remove(SocketHandle, std::uint64_t id) noexcept {
    std::erase_if(impl_->entries, [&](const auto& entry) {
        return entry.id == id;
    });
}

vinc::Result<std::size_t> NativeSocketPoller::wait(std::span<NativeSocketEvent> output, int milliseconds) {
    auto& impl = *impl_;
    impl.descriptors[0] = {impl.reader, POLLRDNORM, 0};
    for (std::size_t i = 0; i < impl.entries.size(); ++i) {
        impl.descriptors[i + 1] = {impl.entries[i].handle, impl.entries[i].interests, 0};
    }

    const int ready = poll_sockets(impl.descriptors.data(), impl.entries.size() + 1, milliseconds);
    if (ready < 0) {
        const int error = last_socket_error();
        if (error == WSAEINTR) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::WouldBlock, vinc::osal::ErrorOperation::WaitSocket});
        }

        return std::unexpected(socket_error(vinc::osal::ErrorOperation::WaitSocket, error));
    }

    if (!ready) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Timeout, vinc::osal::ErrorOperation::WaitSocket});
    }

    if (impl.descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::SystemError, vinc::osal::ErrorOperation::WaitSocket});
    }

    if (impl.descriptors[0].revents & POLLRDNORM) {
        std::lock_guard lock(impl.wake_mutex);
        char byte;
        const int received = ::recv(impl.reader, &byte, 1, 0);
        if (received < 0) {
            return std::unexpected(socket_error(vinc::osal::ErrorOperation::WaitSocket));
        }

        if (!received) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::Closed, vinc::osal::ErrorOperation::WaitSocket});
        }
        impl.wake_pending = false;
    }

    std::size_t count = 0;
    const auto total = impl.entries.size();
    const auto start = total ? impl.cursor % total : 0;
    for (std::size_t offset = 0; offset < total && count < output.size(); ++offset) {
        const auto index = (start + offset) % total;
        const auto flags = impl.descriptors[index + 1].revents;
        if (!flags) {
            continue;
        }

        auto events = SocketEvents::None;
        if (flags & POLLRDNORM) {
            events = events | SocketEvents::Readable;
        }

        if (flags & POLLWRNORM) {
            events = events | SocketEvents::Writable;
        }

        if (flags & (POLLERR | POLLNVAL)) {
            events = events | SocketEvents::Error;
        }

        if (flags & POLLHUP) {
            events = events | SocketEvents::Hangup;
        }
        output[count++] = {impl.entries[index].id, events};
        impl.cursor = index + 1;
    }

    return count;
}

vinc::Result<void> NativeSocketPoller::wake() noexcept {
    std::lock_guard lock(impl_->wake_mutex);
    if (impl_->wake_pending) {
        return {};
    }

    const char byte = 1;
    if (::send(impl_->writer, &byte, 1, 0) != 1) {
        return std::unexpected(socket_error(vinc::osal::ErrorOperation::WakeSocket));
    }
    impl_->wake_pending = true;
    return {};
}
} // namespace vinc::osal::detail
