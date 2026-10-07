#pragma once

#include <vinc/osal/error.hpp>
#include <filesystem>
#include <string>

namespace vinc::osal {
// Owns one native loader reference. Symbols and objects borrowing its code must
// be quiescent before close/destruction. Loading may execute library constructors;
// neither loading nor unloading has a portable timeout or cancellation guarantee.
class DynamicLibrary {
public:
    using Function = void (*)();

    DynamicLibrary() noexcept = default;
    ~DynamicLibrary();
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) = delete;

    // Absolute native path; never search the current directory for the primary library.
    static Result<DynamicLibrary> open(const std::filesystem::path& path);
    Result<Function> function(const std::string& name) const;
    Result<void> close() noexcept;

private:
    explicit DynamicLibrary(void* handle) noexcept : handle_(handle) {}
    void* handle_ = nullptr;
};
}
