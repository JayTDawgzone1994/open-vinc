#pragma once
#include <vinc/detail/runtime_services.hpp>
#include <vinc/module/error.hpp>
#include <vinc/osal/detail/resource_coordinator.hpp>
#include <string>

namespace vinc {
namespace detail {
class ModuleObject;
inline constexpr std::uint32_t plugin_entry_version = 19;
inline constexpr char plugin_entry_symbol[] = "vinc_module_plugin";

// Same-toolchain C++ ABI. The C export only fixes symbol lookup; it does not
// make ModuleObject, Result or STL types portable across incompatible builds.
struct PluginEntry {
    std::uint32_t version;
    std::uint32_t size;
    const char* module_type;
    Result<void> (*initialize)(const RuntimeServices&, const osal::detail::ResourceCoordination&) noexcept;
    Result<ModuleObject*> (*create)();
    void (*destroy)(ModuleObject*) noexcept;
    void (*clear)() noexcept;
};
using PluginQuery = const PluginEntry* (*)() noexcept;

// Binding is one-way for the image lifetime. A failed initialization ends this
// Host; never switch back to a private owner during cleanup/static destruction.
VINC_LOCAL inline Result<void> initialize_plugin(const RuntimeServices& services,
                                                const osal::detail::ResourceCoordination& coordination) noexcept {
    auto bound = osal::detail::bind_resource_coordination(coordination);
    if (!bound) {
        return std::unexpected(bound.error());
    }

    return bind_runtime_services(services);
}
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
            ::vinc::detail::initialize_plugin, \
            []() -> ::vinc::Result<::vinc::detail::ModuleObject*> { \
                return new ClassType(); \
            }, \
            [](::vinc::detail::ModuleObject* module) noexcept { \
                delete static_cast<ClassType*>(module); \
            }, \
            ::vinc::detail::clear_runtime_services}; \
        return &entry; \
    }
