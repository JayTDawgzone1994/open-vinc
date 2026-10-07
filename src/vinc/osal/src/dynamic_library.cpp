#include <vinc/osal/dynamic_library.hpp>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vinc::osal {
namespace {
Error loader_error(ErrorOperation operation, ErrorCode code = ErrorCode::SystemError) noexcept {
#ifdef _WIN32
    return {code, operation, {NativeErrorSource::Win32, GetLastError()}};
#else
    // dlerror is text, not errno. Do not fabricate a native numeric diagnostic.
    return {code, operation};
#endif
}
}

DynamicLibrary::~DynamicLibrary() {
    (void)close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

Result<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.native().find(typename std::filesystem::path::value_type{}) !=
                                   std::filesystem::path::string_type::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::OpenLibrary});
    }

#ifdef _WIN32
    auto handle = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle) {
        return std::unexpected(loader_error(ErrorOperation::OpenLibrary));
    }

    return DynamicLibrary(handle);
}

Result<DynamicLibrary::Function> DynamicLibrary::function(const std::string& name) const {
    if (!handle_ || name.empty() || name.find('\0') != std::string::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::FindSymbol});
    }

#ifdef _WIN32
    auto symbol = GetProcAddress(static_cast<HMODULE>(handle_), name.c_str());
#else
    dlerror();
    auto symbol = dlsym(handle_, name.c_str());
#endif
    if (!symbol) {
        return std::unexpected(loader_error(ErrorOperation::FindSymbol, ErrorCode::NotFound));
    }

    // Native loader boundary: Windows/POSIX support function addresses returned by
    // their loader. The caller must restore the exact exported function signature.
    return reinterpret_cast<Function>(symbol);
}

Result<void> DynamicLibrary::close() noexcept {
    if (!handle_) {
        return {};
    }

#ifdef _WIN32
    const bool closed = FreeLibrary(static_cast<HMODULE>(handle_)) != 0;
#else
    const bool closed = dlclose(handle_) == 0;
#endif
    if (!closed) {
        return std::unexpected(loader_error(ErrorOperation::CloseLibrary));
    }

    handle_ = nullptr;
    return {};
}
}
