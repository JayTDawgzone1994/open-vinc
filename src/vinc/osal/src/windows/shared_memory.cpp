#include "../shared_memory_state.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <vector>
#include <utility>

namespace vinc::osal::detail {
namespace {
OsError error(Operation op, DWORD value = GetLastError()) noexcept {
    ErrorCode code = ErrorCode::SystemError;
    switch (value) {
    case ERROR_ALREADY_EXISTS: code = ErrorCode::AlreadyExists; break;
    case ERROR_FILE_NOT_FOUND: code = ErrorCode::NotFound; break;
    case ERROR_ACCESS_DENIED: code = ErrorCode::PermissionDenied; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT:
        code = ErrorCode::ResourceExhausted; break;
    case ERROR_INVALID_PARAMETER: case ERROR_MAPPED_ALIGNMENT:
        code = ErrorCode::InvalidArgument; break;
    }
    return {code, op, {static_cast<int>(value), std::system_category()}};
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct LocalAllocation {
    void* value = nullptr;
    ~LocalAllocation() { if (value) LocalFree(value); }
};
std::wstring native_name(const std::string& name) {
    return L"Local\\ov_" + std::wstring(name.begin(), name.end());
}
}
SharedMemoryState::~SharedMemoryState() { (void)close_native(); }
Result<void> SharedMemoryState::close_native() noexcept {
    if (handle == -1) return {};
    if (!CloseHandle(reinterpret_cast<HANDLE>(handle))) return std::unexpected(error(Operation::CloseSharedMemory));
    handle = -1;
    return {};
}
SharedMemoryMappingState::~SharedMemoryMappingState() { (void)unmap_native(); }
Result<void> SharedMemoryMappingState::unmap_native() noexcept {
    if (!address) return {};
    if (!UnmapViewOfFile(address)) return std::unexpected(error(Operation::UnmapSharedMemory));
    address = nullptr;
    length = 0;
    return {};
}
Result<void> create_memory(SharedMemoryState& state, const std::string& name, std::uint64_t size) {
    constexpr auto op = Operation::CreateSharedMemory;
    const auto native = native_name(name);
    // Restrict the DACL to this process's user, rather than inheriting an
    // environment-dependent default DACL. Handles are never inheritable.
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return std::unexpected(error(op));
    DWORD required = 0;
    if (!GetTokenInformation(token.value, TokenUser, nullptr, 0, &required) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return std::unexpected(error(op));
    std::vector<std::byte> storage(required);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), required, &required)) return std::unexpected(error(op));
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
        return std::unexpected(error(op));
    LocalAllocation sid_owner{sid};
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return std::unexpected(error(op));
    LocalAllocation descriptor_owner{descriptor};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
        static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xffffffffu), native.c_str());
    const DWORD result = GetLastError();
    if (!handle) return std::unexpected(error(op, result));
    if (result == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle); // do not adopt, truncate, or alter an existing object
        return std::unexpected(error(op, result));
    }
    state.handle = reinterpret_cast<std::intptr_t>(handle);
    return {};
}
Result<void> open_memory(SharedMemoryState& state, const std::string& name, SharedMemoryAccess access) {
    const auto native = native_name(name);
    const DWORD rights = access == SharedMemoryAccess::ReadWrite ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ;
    HANDLE handle = OpenFileMappingW(rights, FALSE, native.c_str());
    if (!handle) return std::unexpected(error(Operation::OpenSharedMemory));
    state.handle = reinterpret_cast<std::intptr_t>(handle);
    return {};
}
Result<void> map_memory(SharedMemoryMappingState& state) {
    const DWORD rights = state.access == SharedMemoryAccess::ReadWrite ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ;
    auto* address = MapViewOfFile(reinterpret_cast<HANDLE>(state.owner->handle), rights, 0, 0, state.length);
    if (!address) return std::unexpected(error(Operation::MapSharedMemory));
    state.address = static_cast<std::byte*>(address);
    return {};
}
Result<void> unlink_memory(const std::string&) {
    return std::unexpected(OsError{ErrorCode::Unsupported, Operation::UnlinkSharedMemory});
}
} // namespace vinc::osal::detail
