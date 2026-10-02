// One-byte kernel lock: OS cleans it up after process exit. Handles are private
// and non-inheritable; omitting FILE_SHARE_DELETE prevents live path replacement.
#include "../file_lock_state.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <vector>

namespace vinc::osal::detail {
namespace {
OsError error(Operation operation, DWORD value = GetLastError()) {
    auto code = ErrorCode::SystemError;
    switch (value) {
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        code = ErrorCode::AlreadyExists;
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        code = ErrorCode::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
        code = ErrorCode::PermissionDenied;
        break;
    case ERROR_LOCK_VIOLATION:
    case ERROR_SHARING_VIOLATION:
        code = ErrorCode::WouldBlock;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_DISK_FULL:
        code = ErrorCode::ResourceExhausted;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_FILENAME_EXCED_RANGE:
        code = ErrorCode::InvalidArgument;
        break;
    case ERROR_NOT_SUPPORTED:
        code = ErrorCode::Unsupported;
        break;
    }

    return {code, operation, std::error_code(static_cast<int>(value), std::system_category())};
}

struct Handle {
    HANDLE value = nullptr;

    ~Handle() {
        if (value) {
            CloseHandle(value);
        }
    }
};

struct Local {
    void* value = nullptr;

    ~Local() {
        if (value) {
            LocalFree(value);
        }
    }
};
}

FileLockState::~FileLockState() {
    if (handle != -1) {
        (void)CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

Result<void> open_file_lock(FileLockState& state, const std::filesystem::path& path, bool create) {
    const auto operation = create ? Operation::CreateFileLock : Operation::OpenFileLock;
    Local descriptor;
    if (create) {
        Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
            return std::unexpected(error(operation));
        }
        DWORD size = 0;
        if (!GetTokenInformation(token.value, TokenUser, nullptr, 0, &size) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::unexpected(error(operation));
        }

        std::vector<std::byte> bytes(size);
        if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) {
            return std::unexpected(error(operation));
        }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) {
            return std::unexpected(error(operation));
        }
        Local sid_owner{sid};
        const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &descriptor.value, nullptr)) {
            return std::unexpected(error(operation));
        }
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(error(operation));
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        auto failure = error(operation);
        CloseHandle(handle);
        return std::unexpected(failure);
    }

    if (GetFileType(handle) != FILE_TYPE_DISK ||
        (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(handle);
        return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
    }
    state.handle = reinterpret_cast<std::intptr_t>(handle);
    return {};
}

Result<void> try_lock_file_lock(FileLockState& state) {
    OVERLAPPED range{};
    if (LockFileEx(reinterpret_cast<HANDLE>(state.handle),
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range)) {
        return {};
    }

    return std::unexpected(error(Operation::LockFileLock));
}

Result<void> unlock_file_lock(FileLockState& state) {
    OVERLAPPED range{};
    if (UnlockFileEx(reinterpret_cast<HANDLE>(state.handle), 0, 1, 0, &range)) {
        return {};
    }

    return std::unexpected(error(Operation::UnlockFileLock));
}
} // namespace vinc::osal::detail
