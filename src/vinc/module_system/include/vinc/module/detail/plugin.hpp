#pragma once
#include <vinc/detail/runtime_services.hpp>
#include <vinc/module/error.hpp>
#include <string>

namespace vinc {
namespace detail {
class ModuleObject;
inline constexpr std::uint32_t plugin_entry_version = 2;
inline constexpr char plugin_entry_symbol[] = "vinc_module_plugin";

// Same-toolchain C++ ABI. The C export only fixes symbol lookup; it does not
// make ModuleObject, Result or STL types portable across incompatible builds.
struct PluginEntry {
    std::uint32_t version;
    std::uint32_t size;
    const char* module_type;
    Result<void> (*initialize)(const RuntimeServices&) noexcept;
    Result<ModuleObject*> (*create)(std::string);
    void (*destroy)(ModuleObject*) noexcept;
    void (*clear)() noexcept;
};
using PluginQuery = const PluginEntry* (*)() noexcept;
}
}

#ifdef _WIN32
#define VINC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define VINC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// No application factory or registration runs during loader initialization.
// Exactly one export per Plugin; creation and destruction execute in this image.
#define REGISTER_MODULE(ClassType, ModuleTypeName) \
    extern "C" VINC_PLUGIN_EXPORT const ::vinc::detail::PluginEntry* vinc_module_plugin() noexcept { \
        static const ::vinc::detail::PluginEntry entry{ \
            ::vinc::detail::plugin_entry_version, sizeof(::vinc::detail::PluginEntry), ModuleTypeName, \
            ::vinc::detail::bind_runtime_services, \
            [](std::string name) -> ::vinc::Result<::vinc::detail::ModuleObject*> { \
                return new ClassType(std::move(name)); \
            }, \
            [](::vinc::detail::ModuleObject* module) noexcept { \
                delete static_cast<ClassType*>(module); \
            }, \
            ::vinc::detail::clear_runtime_services}; \
        return &entry; \
    }
