#include <vinc/osal/detail/notification_state.hpp>
#include <vinc/osal/detail/native_error.hpp>
#include <vinc/osal/detail/posix/descriptor.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vinc::osal::detail {
namespace {
Error error(ErrorOperation operation, int value = errno) noexcept {
    auto result = native_error(operation, value);
    switch (value) {
    case EEXIST:
        result.code = static_cast<std::int32_t>(ErrorCode::AlreadyExists);
        break;
    case EPIPE:
    case ENXIO:
        result.code = static_cast<std::int32_t>(ErrorCode::PeerClosed);
        break;
    case ELOOP:
    case ENAMETOOLONG:
        result.code = static_cast<std::int32_t>(ErrorCode::InvalidArgument);
        break;
    case ENOSPC:
    case EDQUOT:
        result.code = static_cast<std::int32_t>(ErrorCode::ResourceExhausted);
        break;
    }
    return result;
}

struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {
    }
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (value >= 0) {
            // close(EINTR) may already have released/reused the number. No retry.
            (void)::close(value);
        }
    }
    int release() noexcept {
        return std::exchange(value, -1);
    }
};

Result<Fd> open_directory(bool create, ErrorOperation operation) {
    // A stable per-effective-user namespace, independent of HOME/TMPDIR and
    // framework instances. Only this leaf is created; never follow a leaf link.
    const auto path = "/tmp/ov-osal-notify-" + std::to_string(::geteuid());
    if (create && ::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return std::unexpected(error(operation));
    }
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0) {
        return std::unexpected(error(operation));
    }
    auto isolated = isolate_descriptor(directory.value, operation);
    if (!isolated) {
        return std::unexpected(isolated.error());
    }
    struct stat info{};
    if (::fstat(directory.value, &info) != 0) {
        return std::unexpected(error(operation));
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
        return std::unexpected(Error{ErrorCode::PermissionDenied, operation});
    }
    return directory;
}

bool valid_fifo(const struct stat& info) noexcept {
    return S_ISFIFO(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0;
}

Result<struct stat> inspect_name(int directory, const std::string& name, ErrorOperation operation) {
    struct stat info{};
    if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(error(operation));
    }
    if (!valid_fifo(info)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, operation});
    }
    return info;
}

Result<Fd> open_endpoint(int directory, const std::string& name, int access,
                         const struct stat& identity, ErrorOperation operation) {
    Fd endpoint(::openat(directory, name.c_str(), access | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (endpoint.value < 0) {
        return std::unexpected(error(operation));
    }
    auto isolated = isolate_descriptor(endpoint.value, operation);
    if (!isolated) {
        return std::unexpected(isolated.error());
    }
    struct stat info{};
    if (::fstat(endpoint.value, &info) != 0) {
        return std::unexpected(error(operation));
    }
    if (!valid_fifo(info) || info.st_dev != identity.st_dev || info.st_ino != identity.st_ino) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, operation});
    }
#ifdef VINC_OSAL_MACOS
    // Darwin pipe SIGPIPE can be process-directed. Suppress it on this private
    // open-file description instead of changing any process-global disposition.
    if (access == O_WRONLY && ::fcntl(endpoint.value, F_SETNOSIGPIPE, 1) < 0) {
        return std::unexpected(error(operation));
    }
#endif
    return endpoint;
}

// Publish both descriptors into state only after both opens succeed. On failure
// the local owners close descriptors without unlinking or consuming FIFO data.
Result<void> open_receiver_endpoints(NotificationState& state, int directory,
                                     const std::string& name, const struct stat& identity,
                                     ErrorOperation operation) {
    auto reader = open_endpoint(directory, name, O_RDONLY, identity, operation);
    if (!reader) {
        return std::unexpected(reader.error());
    }

    auto writer = open_endpoint(directory, name, O_WRONLY, identity, operation);
    if (!writer) {
        return std::unexpected(writer.error());
    }

    state.handle = reader->release();
    state.writer = writer->release();
    return {};
}

struct CreatedName {
    int directory;
    const std::string& name;
    bool committed = false;
    ~CreatedName() {
        if (!committed) {
            // Only a fresh, unpublished name belongs to this rollback. No open
            // failure is permitted to unlink somebody else's existing object.
            (void)::unlinkat(directory, name.c_str(), 0);
        }
    }
};

Result<void> write_token(int descriptor) noexcept {
    constexpr unsigned char token = 1;
    ssize_t written;
    do {
        written = ::write(descriptor, &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
    if (written == sizeof(token)) {
        return {};
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // A bounded full FIFO already contains a hint. The receiver must inspect
        // actual work after consuming hints; exact token counts are irrelevant.
        return {};
    }
    if (written < 0) {
        return std::unexpected(error(ErrorOperation::Notify));
    }
    return std::unexpected(Error{ErrorCode::SystemError, ErrorOperation::Notify});
}

int wait_milliseconds(Deadline deadline) noexcept {
    if (deadline == (Deadline::max)()) {
        return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>((std::min)(remaining, static_cast<std::int64_t>(INT_MAX)));
}
}

NotificationState::~NotificationState() {
    (void)close_native();
}

Result<void> NotificationState::close_native() noexcept {
    std::optional<Error> failure;
    for (auto* slot : {&writer, &handle}) {
        const auto descriptor = std::exchange(*slot, -1);
        if (descriptor >= 0 && ::close(static_cast<int>(descriptor)) != 0 && !failure) {
            failure = error(ErrorOperation::CloseNotification);
        }
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return {};
}

Result<void> create_notification(NotificationState& state, std::string_view logical_name) {
    constexpr auto operation = ErrorOperation::CreateNotification;
    const std::string name(logical_name);
    auto directory = open_directory(true, operation);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    if (::mkfifoat(directory->value, name.c_str(), 0600) != 0) {
        return std::unexpected(error(operation));
    }
    CreatedName created{directory->value, name};
    auto identity = inspect_name(directory->value, name, operation);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    auto opened = open_receiver_endpoints(state, directory->value, name, *identity, operation);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    created.committed = true;
    return {};
}

Result<void> open_notification_receiver(NotificationState& state, std::string_view logical_name) {
    constexpr auto operation = ErrorOperation::OpenNotification;
    const std::string name(logical_name);
    auto directory = open_directory(false, operation);
    if (!directory) {
        return std::unexpected(directory.error());
    }

    auto identity = inspect_name(directory->value, name, operation);
    if (!identity) {
        return std::unexpected(identity.error());
    }

    return open_receiver_endpoints(state, directory->value, name, *identity, operation);
}

Result<void> open_notification(NotificationState& state, std::string_view logical_name) {
    constexpr auto operation = ErrorOperation::OpenNotification;
    const std::string name(logical_name);
    auto directory = open_directory(false, operation);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    auto identity = inspect_name(directory->value, name, operation);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    auto writer = open_endpoint(directory->value, name, O_WRONLY, *identity, operation);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    state.handle = writer->release();
    return {};
}

Result<void> unlink_notification(std::string_view logical_name) {
    constexpr auto operation = ErrorOperation::UnlinkNotification;
    const std::string name(logical_name);
    auto directory = open_directory(false, operation);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    auto identity = inspect_name(directory->value, name, operation);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    // Names are never reused while late cleanup remains possible. Same-user
    // malicious namespace replacement is outside this cooperative IPC contract.
    if (::unlinkat(directory->value, name.c_str(), 0) != 0) {
        return std::unexpected(error(operation));
    }
    return {};
}

Result<void> signal_notification(const NotificationState& state) noexcept {
#ifdef VINC_OSAL_MACOS
    return write_token(static_cast<int>(state.handle));
#else
    // Only suppress the SIGPIPE generated by this write. Preserve the caller's
    // mask and any SIGPIPE already pending; do not install a process handler.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    sigset_t previous;
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (mask_error != 0) {
        return std::unexpected(error(ErrorOperation::Notify, mask_error));
    }
    sigset_t pending;
    if (::sigpending(&pending) != 0) {
        auto failure = error(ErrorOperation::Notify);
        (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        return std::unexpected(failure);
    }
    const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
    auto result = write_token(static_cast<int>(state.handle));
    if (!result && result.error().native.code == EPIPE && !already_pending) {
        // Zero-time wait drains only our newly generated signal. EAGAIN is
        // normal when SIGPIPE was ignored and therefore never became pending.
        const timespec no_wait{};
        int received;
        do {
            received = ::sigtimedwait(&blocked, nullptr, &no_wait);
        } while (received < 0 && errno == EINTR);
    }
    const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (restore_error != 0) {
        return std::unexpected(error(ErrorOperation::Notify, restore_error));
    }
    return result;
#endif
}

Result<void> wake_notification(const NotificationState& state) noexcept {
    // The receiver's read end stays alive until stop_callback joins. Unlike an
    // external sender, this self-writer cannot encounter peer death during wait.
    return write_token(static_cast<int>(state.writer));
}

Result<void> wait_notification(NotificationState& state, Deadline deadline, std::stop_token stop) noexcept {
    constexpr auto operation = ErrorOperation::WaitNotification;
    // A fixed stack read bounds work even if senders keep filling the FIFO.
    constexpr std::size_t token_read_bytes = 256;
    std::array<unsigned char, token_read_bytes> tokens;
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, operation});
        }
        pollfd item{static_cast<int>(state.handle), POLLIN, 0};
        const int ready = ::poll(&item, 1, wait_milliseconds(deadline));
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, operation});
        }
        if (ready < 0 && errno != EINTR) {
            return std::unexpected(error(operation));
        }
        if (ready > 0) {
            if (item.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return std::unexpected(Error{ErrorCode::SystemError, operation});
            }
            if (item.revents & POLLIN) {
                const auto count = ::read(item.fd, tokens.data(), tokens.size());
                if (count > 0) {
                    return {};
                }
                if (count == 0) {
                    return std::unexpected(Error{ErrorCode::PeerClosed, operation});
                }
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    return std::unexpected(error(operation));
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, operation});
        }
    }
}
} // namespace vinc::osal::detail
