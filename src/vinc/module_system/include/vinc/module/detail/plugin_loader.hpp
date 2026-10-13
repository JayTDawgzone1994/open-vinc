#pragma once
#include <vinc/module/detail/plugin.hpp>
#include <vinc/osal/dynamic_library.hpp>
#include <memory>

namespace vinc::detail {
using PluginModule = std::unique_ptr<ModuleObject, void (*)(ModuleObject*) noexcept>;

// Declare this owner before RuntimeGuard. Module objects die first, then Host
// dispatch/Client callbacks stop, then Plugin binding clears and library closes.
class LoadedPlugin {
public:
    ~LoadedPlugin();
    LoadedPlugin(LoadedPlugin&& other) noexcept;
    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    static Result<LoadedPlugin> load(const std::filesystem::path& path,
                                     const std::string& type, const RuntimeServices& services);
    Result<PluginModule> create() const;

private:
    LoadedPlugin(osal::DynamicLibrary library, const PluginEntry* entry) noexcept;
    osal::DynamicLibrary library_;
    const PluginEntry* entry_ = nullptr;
};
}
