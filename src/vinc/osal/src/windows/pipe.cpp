// Windows Pipe backend using private named-pipe endpoints and overlapped I/O.
// IOCP completion, not a cancel request, ends the native operation lifetime.

#include <vinc/osal/detail/operation_error.hpp>
#include <vinc/osal/detail/pipe_native.hpp>
#include <vinc/osal/random.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <charconv>
#include <limits>
#include <type_traits>
#include <utility>

namespace vinc::osal::detail {
namespace {
struct Handle {
    HANDLE value = nullptr;

    explicit Handle(HANDLE handle = nullptr) noexcept : value(handle) {
    }

    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }

    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {
    }

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Handle old(std::exchange(value, std::exchange(other.value, nullptr)));
        }

        return *this;
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct LocalMemory {
    void* value = nullptr;

    ~LocalMemory() {
        if (value) {
            LocalFree(value);
        }
    }
};

vinc::Error win_error(vinc::osal::ErrorOperation operation) noexcept {
    return native_error(operation, static_cast<int>(GetLastError()));
}

vinc::Result<void> owner_security(LocalMemory& descriptor) {
    HANDLE raw_token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    Handle token(raw_token);
    DWORD length = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
    std::vector<std::byte> storage(length);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), length, &length)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    LPWSTR text = nullptr;
    // GetTokenInformation filled a buffer sized by its length query. The
    // default allocator meets TOKEN_USER alignment on Windows; its embedded
    // SID points into this live buffer and is consumed before it is freed.
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    LocalMemory sid{text};
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(text) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor.value, nullptr)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }

    return {};
}
}

struct PipeMonitorImpl {
    Handle port;
    std::atomic<bool> wake_pending{false};
};

struct NativePipeImpl {
    Handle handle;
    bool attached = false;
};

struct NativeTransferImpl {
    OVERLAPPED overlapped{}; // first member: address is the completion identity
    HANDLE handle = nullptr;
    IoKind kind = IoKind::Read;
    bool pending = false;
    bool done = false;
    NativeIoResult result{IoStatus::Failed};
};

static_assert(std::is_standard_layout_v<NativeTransferImpl>);

PipeMonitor::PipeMonitor(std::unique_ptr<PipeMonitorImpl> impl) : impl_(std::move(impl)) {
}

PipeMonitor::~PipeMonitor() = default;

vinc::Result<std::unique_ptr<PipeMonitor>> PipeMonitor::create() {
    auto impl = std::make_unique<PipeMonitorImpl>();
    impl->port = Handle(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!impl->port.value) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::InitializeService));
    }

    return std::unique_ptr<PipeMonitor>(new PipeMonitor(std::move(impl)));
}

void PipeMonitor::wake() noexcept {
    if (!impl_->wake_pending.exchange(true)) {
        if (!PostQueuedCompletionStatus(impl_->port.value, 0, 0, nullptr)) {
            impl_->wake_pending.store(false);
        }
    }
}

vinc::Result<void> PipeMonitor::initialize_worker() noexcept {
    return {};
}

vinc::Result<void> PipeMonitor::wait(int milliseconds) noexcept {
    DWORD transferred = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL success =
        GetQueuedCompletionStatus(impl_->port.value, &transferred, &key, &overlapped,
                                  milliseconds < 0 ? INFINITE : static_cast<DWORD>(milliseconds));
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (!overlapped) {
        if (success) {
            impl_->wake_pending.store(false);
        }

        if (success || error == WAIT_TIMEOUT) {
            return {};
        }

        return std::unexpected(native_error(vinc::osal::ErrorOperation::WaitIo, static_cast<int>(error)));
    }
    // NativeTransferImpl is standard-layout (asserted above), so its first
    // member and enclosing object are pointer-interconvertible. The transfer
    // remains alive until IOCP delivers completion, including after cancellation.
    auto& transfer = *reinterpret_cast<NativeTransferImpl*>(overlapped);
    transfer.pending = false;
    transfer.done = true;
    transfer.result.transferred = transferred;
    const auto operation = transfer.kind == IoKind::Read ? vinc::osal::ErrorOperation::ReadPipe
                                                         : vinc::osal::ErrorOperation::WritePipe;
    if (success) {
        transfer.result.status =
            transferred == 0 && transfer.kind == IoKind::Read ? IoStatus::EndOfStream : IoStatus::Completed;
    } else if (error == ERROR_OPERATION_ABORTED) {
        transfer.result.status = IoStatus::Cancelled;
    } else if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
        transfer.result.status = transfer.kind == IoKind::Read ? IoStatus::EndOfStream : IoStatus::PeerClosed;
    } else {
        transfer.result.status = IoStatus::Failed;
        transfer.result.error = native_error(operation, static_cast<int>(error));
    }

    return {};
}

NativePipe::NativePipe(std::unique_ptr<NativePipeImpl> impl) : impl_(std::move(impl)) {
}

NativePipe::~NativePipe() = default;

vinc::Result<NativePipePair> NativePipe::create_pair(bool blocking_writer) {
    std::array<std::byte, 16> random{};
    auto generated = random_bytes(random);
    if (!generated) {
        return std::unexpected(operation_error(ErrorOperation::CreatePipe, generated.error()));
    }

    std::wstring name = L"\\\\.\\pipe\\vinc-osal-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto value : random) {
        const auto byte = std::to_integer<unsigned char>(value);
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    LocalMemory security;
    auto secured = owner_security(security);
    if (!secured) {
        return std::unexpected(secured.error());
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.value, FALSE};
    auto parent = std::make_unique<NativePipeImpl>();
    auto child = std::make_unique<NativePipeImpl>();
    parent->handle = Handle(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
        &attributes));
    if (parent->handle.value == INVALID_HANDLE_VALUE) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    // Connect a private read end and write end. Neither end is inheritable
    // or attached to a service until explicitly used or handed off.
    SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    child->handle = Handle(CreateFileW(name.c_str(), GENERIC_WRITE, 0, &inherited, OPEN_EXISTING,
                                       blocking_writer ? 0 : FILE_FLAG_OVERLAPPED, nullptr));
    if (child->handle.value == INVALID_HANDLE_VALUE) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    Handle connected_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected_event.value) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }
    OVERLAPPED connected{};
    connected.hEvent = connected_event.value;
    if (!ConnectNamedPipe(parent->handle.value, &connected)) {
        const auto error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD ignored;
            if (!GetOverlappedResult(parent->handle.value, &connected, &ignored, TRUE)) {
                return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
            }
        } else if (error != ERROR_PIPE_CONNECTED) {
            return std::unexpected(
                native_error(vinc::osal::ErrorOperation::CreatePipe, static_cast<int>(error)));
        }
    }
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(parent->handle.value, &client_pid)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::CreatePipe));
    }

    if (client_pid != GetCurrentProcessId()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::PermissionDenied, vinc::osal::ErrorOperation::CreatePipe});
    }

    return NativePipePair{std::unique_ptr<NativePipe>(new NativePipe(std::move(parent))),
                          std::unique_ptr<NativePipe>(new NativePipe(std::move(child)))};
}

vinc::Result<void> NativePipe::attach(PipeMonitor& monitor) {
    if (!impl_->attached) {
        if (!CreateIoCompletionPort(impl_->handle.value, monitor.implementation().port.value, 0, 0)) {
            return std::unexpected(win_error(vinc::osal::ErrorOperation::AdoptPipe));
        }
        impl_->attached = true;
    }

    return {};
}

std::intptr_t NativePipe::handle() const noexcept {
    // Windows HANDLE is an opaque pointer-sized token. intptr_t preserves
    // its value for this native boundary; no pointed-to object is accessed.
    return reinterpret_cast<std::intptr_t>(impl_->handle.value);
}

vinc::Result<void> NativePipe::validate_handle(std::intptr_t value, IoKind) {
    // Windows HANDLE is an opaque pointer-sized token. intptr_t preserves
    // its value for this native boundary; no pointed-to object is accessed.
    HANDLE handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    if (!value || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_PIPE ||
        !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
    }

    return {};
}

vinc::Result<std::unique_ptr<NativePipe>> NativePipe::adopt_handle(std::intptr_t value, IoKind) {
    auto native = std::unique_ptr<NativePipe>(new NativePipe(std::make_unique<NativePipeImpl>()));
    // Windows HANDLE is an opaque pointer-sized token. intptr_t preserves
    // its value for this native boundary; no pointed-to object is accessed.
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(value), HANDLE_FLAG_INHERIT, 0)) {
        return std::unexpected(win_error(vinc::osal::ErrorOperation::AdoptPipe));
    }
    // Windows HANDLE is an opaque pointer-sized token. intptr_t preserves
    // its value for this native boundary; no pointed-to object is accessed.
    native->impl_->handle = Handle(reinterpret_cast<HANDLE>(value));
    return native;
}

NativeTransfer::NativeTransfer() : impl_(std::make_unique<NativeTransferImpl>()) {
}

NativeTransfer::~NativeTransfer() = default; // owner waits for IOCP completion first

void NativeTransfer::start(NativePipe& pipe, PipeMonitor&, std::span<std::byte> buffer,
                           IoKind kind) noexcept {
    impl_->handle = pipe.implementation().handle.value;
    impl_->kind = kind;
    const auto size = static_cast<DWORD>(buffer.size());
    const BOOL success = kind == IoKind::Read
                             ? ReadFile(impl_->handle, buffer.data(), size, nullptr, &impl_->overlapped)
                             : WriteFile(impl_->handle, buffer.data(), size, nullptr, &impl_->overlapped);
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (success || error == ERROR_IO_PENDING) {
        // Even synchronous success queues a completion packet. Never release
        // OVERLAPPED early or manufacture a second completion for that packet.
        impl_->pending = true;
    } else {
        impl_->done = true;
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
            impl_->result.status = kind == IoKind::Read ? IoStatus::EndOfStream : IoStatus::PeerClosed;
        } else {
            impl_->result.status = IoStatus::Failed;
            impl_->result.error = native_error(kind == IoKind::Read ? vinc::osal::ErrorOperation::ReadPipe
                                                                    : vinc::osal::ErrorOperation::WritePipe,
                                               static_cast<int>(error));
        }
    }
}

std::optional<NativeIoResult> NativeTransfer::poll() noexcept {
    return impl_->done ? std::optional(impl_->result) : std::nullopt;
}

void NativeTransfer::cancel() noexcept {
    if (impl_->pending) {
        CancelIoEx(impl_->handle, &impl_->overlapped);
    }
}
} // namespace vinc::osal::detail
