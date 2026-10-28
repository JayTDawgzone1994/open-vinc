#include <vinc/osal/detail/notification_state.hpp>
#include <algorithm>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

namespace vinc::osal::detail {
namespace {
Error error(ErrorOperation operation, DWORD value = GetLastError()) noexcept {
    auto code = ErrorCode::SystemError;
    switch (value) {
    case ERROR_ALREADY_EXISTS:
        code = ErrorCode::AlreadyExists;
        break;
    case ERROR_FILE_NOT_FOUND:
        code = ErrorCode::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
        code = ErrorCode::PermissionDenied;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
        code = ErrorCode::ResourceExhausted;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_HANDLE:
        code = ErrorCode::InvalidArgument;
        break;
    }
    return {code, operation, {NativeErrorSource::Win32, value}};
}

// Windows HANDLE is an opaque pointer-sized token, stored without dereferencing
// it. intptr_t preserves its native representation at this ABI boundary.
HANDLE native_handle(const NotificationState& state) noexcept {
    return reinterpret_cast<HANDLE>(state.handle);
}

std::wstring native_name(std::string_view name) {
    return L"Local\\ov_notify_" + std::wstring(name.begin(), name.end());
}

struct HandleOwner {
    HANDLE value = nullptr;
    ~HandleOwner() {
        if (value) {
            CloseHandle(value);
        }
    }
};

struct LocalDeleter {
    void operator()(void* value) const noexcept {
        LocalFree(value);
    }
};

// Keep the sized token query and SID-string allocation within one conversion
// boundary; neither native buffer escapes into the returned owned string.
Result<std::wstring> access_rule_from_token(HANDLE token) {
    constexpr auto operation = ErrorOperation::CreateNotification;
    DWORD required = 0;
    if (!GetTokenInformation(token, TokenUser, nullptr, 0, &required) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::unexpected(error(operation));
    }
    std::vector<std::byte> storage(required);
    if (!GetTokenInformation(token, TokenUser, storage.data(), required, &required)) {
        return std::unexpected(error(operation));
    }
    LPWSTR sid = nullptr;
    // The native query supplies a TOKEN_USER in a buffer sized by its first
    // call. Default allocation meets TOKEN_USER alignment; SID lives in storage.
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) {
        return std::unexpected(error(operation));
    }
    std::unique_ptr<void, LocalDeleter> sid_owner(sid);
    return std::wstring(L"D:P(A;;GA;;;") + sid + L")";
}

Result<std::wstring> user_access_rule() {
    HandleOwner token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        return std::unexpected(error(ErrorOperation::CreateNotification));
    }

    return access_rule_from_token(token.value);
}

Result<void> create_event(NotificationState& state, const std::wstring& name,
                          const std::wstring& rule) {
    constexpr auto operation = ErrorOperation::CreateNotification;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(rule.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        return std::unexpected(error(operation));
    }
    std::unique_ptr<void, LocalDeleter> descriptor_owner(descriptor);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateEventW(&attributes, FALSE, FALSE, name.c_str());
    const auto last_error = GetLastError();
    if (!handle) {
        return std::unexpected(error(operation, last_error));
    }
    if (last_error == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return std::unexpected(error(operation, last_error));
    }
    // Opaque native HANDLE, not an address of a C++ object; see native_handle.
    state.handle = reinterpret_cast<std::intptr_t>(handle);
    return {};
}

// Receiver opens need SYNCHRONIZE for waiting and EVENT_MODIFY_STATE for the
// cancellation wake. Sender opens retain only their existing signalling right.
Result<void> open_event(NotificationState& state, std::string_view name, DWORD access) {
    const auto native = native_name(name);
    HANDLE handle = OpenEventW(access, FALSE, native.c_str());
    if (!handle) {
        return std::unexpected(error(ErrorOperation::OpenNotification));
    }

    // Opaque native HANDLE, not an address of a C++ object; see native_handle.
    state.handle = reinterpret_cast<std::intptr_t>(handle);
    return {};
}

DWORD wait_milliseconds(Deadline deadline) noexcept {
    if (deadline == (Deadline::max)()) {
        return INFINITE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    constexpr auto largest_finite_wait = static_cast<std::int64_t>(INFINITE - 1);
    return static_cast<DWORD>((std::min)(remaining, largest_finite_wait));
}
}

NotificationState::~NotificationState() {
    (void)close_native();
}

Result<void> NotificationState::close_native() noexcept {
    if (handle == -1) {
        return {};
    }
    if (!CloseHandle(native_handle(*this))) {
        return std::unexpected(error(ErrorOperation::CloseNotification));
    }
    handle = -1;
    return {};
}

Result<void> create_notification(NotificationState& state, std::string_view name) {
    auto rule = user_access_rule();
    if (!rule) {
        return std::unexpected(rule.error());
    }
    return create_event(state, native_name(name), *rule);
}

Result<void> open_notification(NotificationState& state, std::string_view name) {
    return open_event(state, name, EVENT_MODIFY_STATE);
}

Result<void> open_notification_receiver(NotificationState& state, std::string_view name) {
    return open_event(state, name, SYNCHRONIZE | EVENT_MODIFY_STATE);
}

Result<void> unlink_notification(std::string_view) {
    return std::unexpected(Error{ErrorCode::Unsupported, ErrorOperation::UnlinkNotification});
}

Result<void> signal_notification(const NotificationState& state) noexcept {
    if (!SetEvent(native_handle(state))) {
        return std::unexpected(error(ErrorOperation::Notify));
    }
    return {};
}

Result<void> wake_notification(const NotificationState& state) noexcept {
    return signal_notification(state);
}

Result<void> wait_notification(NotificationState& state, Deadline deadline, std::stop_token stop) noexcept {
    constexpr auto operation = ErrorOperation::WaitNotification;
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, operation});
        }
        const auto result = WaitForSingleObject(native_handle(state), wait_milliseconds(deadline));
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, operation});
        }
        if (result == WAIT_OBJECT_0) {
            return {};
        }
        if (result == WAIT_FAILED) {
            return std::unexpected(error(operation));
        }
        if (result != WAIT_TIMEOUT) {
            return std::unexpected(Error{ErrorCode::SystemError, operation});
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, operation});
        }
    }
}
} // namespace vinc::osal::detail
