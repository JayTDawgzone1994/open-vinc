#include "pipe_platform.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vinc::osal::detail {
Result<void> isolate_pipe_descriptor(int& fd, Operation operation) noexcept {
    if (fd >= 5) return {};
    const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 5);
    if (replacement < 0) return std::unexpected(native_error(operation, errno));
    ::close(fd);
    fd = replacement;
    return {};
}
namespace {
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) noexcept : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { Fd old(std::exchange(value, std::exchange(other.value, -1))); }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
Result<std::array<Fd, 2>> make_pipe() {
    int descriptors[2];
#if defined(VINC_OSAL_MACOS)
    if (::pipe(descriptors) < 0) return std::unexpected(native_error(Operation::CreatePipe, errno));
#else
    if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) < 0) return std::unexpected(native_error(Operation::CreatePipe, errno));
#endif
    std::array<Fd, 2> result{Fd(descriptors[0]), Fd(descriptors[1])};
    // Internal pipe ends must not accidentally become the parent's missing
    // stdin/stdout/stderr, which spawn would otherwise treat as inherited stdio.
    for (auto& fd : result) {
        auto isolated = isolate_pipe_descriptor(fd.value, Operation::CreatePipe);
        if (!isolated) return std::unexpected(isolated.error());
    }
#if defined(VINC_OSAL_MACOS)
    for (auto& fd : result) {
        if (fcntl(fd.value, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd.value, F_SETFL, O_NONBLOCK) < 0)
            return std::unexpected(native_error(Operation::CreatePipe, errno));
    }
#endif
    return result;
}
}
PipeMonitorImpl::~PipeMonitorImpl() { if (wake >= 0) ::close(wake); if (queue >= 0) ::close(queue); }
PipeMonitor::PipeMonitor(std::unique_ptr<PipeMonitorImpl> impl) : impl_(std::move(impl)) {}
PipeMonitor::~PipeMonitor() = default;
Result<void> PipeMonitor::initialize_worker() noexcept {
    // Only this dedicated I/O thread writes pipes. Do not replace the host's
    // SIGPIPE disposition or alter the caller's signal mask.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    const int error = pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (error) return std::unexpected(native_error(Operation::InitializeService, error));
    return {};
}
struct NativePipeImpl { Fd endpoint; };
NativePipe::NativePipe(std::unique_ptr<NativePipeImpl> impl) : impl_(std::move(impl)) {}
NativePipe::~NativePipe() = default;
Result<NativePipePair> NativePipe::create_pair() {
    auto ends = make_pipe();
    if (!ends) return std::unexpected(ends.error());
    auto reader = std::make_unique<NativePipeImpl>();
    auto writer = std::make_unique<NativePipeImpl>();
    reader->endpoint = std::move((*ends)[0]); writer->endpoint = std::move((*ends)[1]);
    return NativePipePair{std::unique_ptr<NativePipe>(new NativePipe(std::move(reader))),
        std::unique_ptr<NativePipe>(new NativePipe(std::move(writer)))};
}
Result<void> NativePipe::attach(PipeMonitor&) { return {}; }
std::intptr_t NativePipe::handle() const noexcept { return impl_->endpoint.value; }
Result<void> NativePipe::validate_handle(std::intptr_t value, IoKind kind) {
    const int fd = static_cast<int>(value);
    struct stat info{};
    const int flags = fcntl(fd, F_GETFL);
    if (value < 3 || value > std::numeric_limits<int>::max() ||
        fstat(fd, &info) < 0 || !S_ISFIFO(info.st_mode) || flags < 0 ||
        (flags & O_ACCMODE) != (kind == IoKind::Read ? O_RDONLY : O_WRONLY) || !(flags & O_NONBLOCK))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
    return {};
}
Result<std::unique_ptr<NativePipe>> NativePipe::adopt_handle(std::intptr_t value, IoKind) {
    auto native = std::unique_ptr<NativePipe>(new NativePipe(std::make_unique<NativePipeImpl>()));
    const int fd = static_cast<int>(value);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(native_error(Operation::AdoptPipe, errno));
    native->impl_->endpoint = Fd(fd);
    return native;
}
struct NativeTransferImpl {
    int fd = -1;
    PipeMonitorImpl* monitor = nullptr;
    IoKind kind = IoKind::Read;
    std::span<std::byte> buffer;
    bool armed = false;
    std::optional<NativeIoResult> result;
    void complete(NativeIoResult value) noexcept {
        result = value;
        if (armed) { disarm_pipe(*monitor, fd, kind); armed = false; }
    }
};
NativeTransfer::NativeTransfer() : impl_(std::make_unique<NativeTransferImpl>()) {}
NativeTransfer::~NativeTransfer() = default;
void NativeTransfer::start(NativePipe& pipe, PipeMonitor& monitor, std::span<std::byte> buffer, IoKind kind) noexcept {
    impl_->fd = pipe.implementation().endpoint.value;
    impl_->monitor = &monitor.implementation(); impl_->kind = kind; impl_->buffer = buffer;
}
std::optional<NativeIoResult> NativeTransfer::poll() noexcept {
    if (impl_->result) return impl_->result;
    ssize_t transferred;
    do {
        transferred = impl_->kind == IoKind::Read ? ::read(impl_->fd, impl_->buffer.data(), impl_->buffer.size())
            : ::write(impl_->fd, impl_->buffer.data(), impl_->buffer.size());
    } while (transferred < 0 && errno == EINTR);
    const auto operation = impl_->kind == IoKind::Read ? Operation::ReadPipe : Operation::WritePipe;
    if (transferred >= 0) {
        impl_->complete({transferred == 0 && impl_->kind == IoKind::Read ? IoStatus::EndOfStream : IoStatus::Completed,
            static_cast<std::size_t>(transferred)});
    } else if (errno == EPIPE) {
        impl_->complete({IoStatus::PeerClosed});
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!impl_->armed) {
            auto result = arm_pipe(*impl_->monitor, impl_->fd, impl_->kind);
            if (!result) impl_->complete({IoStatus::Failed, 0, result.error()});
            else impl_->armed = true;
        }
    } else impl_->complete({IoStatus::Failed, 0, native_error(operation, errno)});
    return impl_->result;
}
void NativeTransfer::cancel() noexcept {
    if (!impl_->result) impl_->complete({IoStatus::Cancelled});
}
} // namespace vinc::osal::detail
