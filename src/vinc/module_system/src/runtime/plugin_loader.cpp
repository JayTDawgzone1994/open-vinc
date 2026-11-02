#include <vinc/module/detail/plugin_loader.hpp>
#include <vinc/module/base_module.hpp>
#include <utility>

namespace vinc::detail {
using module_system::ErrorCode;
using module_system::ErrorOperation;

LoadedPlugin::LoadedPlugin(osal::DynamicLibrary library, const PluginEntry* entry) noexcept
    : library_(std::move(library)), entry_(entry) {}

LoadedPlugin::LoadedPlugin(LoadedPlugin&& other) noexcept
    : library_(std::move(other.library_)), entry_(std::exchange(other.entry_, nullptr)) {}

LoadedPlugin::~LoadedPlugin() {
    if (entry_) {
        entry_->clear();
    }
    // Static destructors use fatal fallback but retain Host resource coordination.
    // Its process-lifetime owner/code remain alive through the native unload.
    // All application work must have stopped before this point.
}

Result<LoadedPlugin> LoadedPlugin::load(const std::filesystem::path& path,
                                      const std::string& type, const RuntimeServices& services,
                                      std::optional<osal::DynamicLibrary>& rejected_library) {
    if (rejected_library) {
        return std::unexpected(Error{ErrorCode::ServiceFailed, ErrorOperation::InitializePlugin});
    }

    auto coordination = osal::detail::resource_coordination();
    if (!coordination) {
        return std::unexpected(wrap_error(ErrorCode::ServiceFailed, ErrorOperation::InitializePlugin,
                                          coordination.error()));
    }

    auto library = osal::DynamicLibrary::open(path);
    if (!library) {
        return std::unexpected(wrap_error(ErrorCode::PluginLoadFailed, ErrorOperation::LoadPlugin,
                                          library.error()));
    }

    // Native loading can execute constructors before validation/injection. If
    // rejected, retain the mapping until Host _Exit rather than unloading code
    // which may already own unknown threads. The caller reports before exiting.
    struct RejectionGuard {
        osal::DynamicLibrary& library;
        std::optional<osal::DynamicLibrary>& rejected;
        bool accepted = false;
        ~RejectionGuard() {
            if (!accepted) {
                rejected.emplace(std::move(library));
            }
        }
    } rejection{*library, rejected_library};

    auto symbol = library->function(plugin_entry_symbol);
    if (!symbol) {
        return std::unexpected(wrap_error(ErrorCode::InvalidPlugin, ErrorOperation::LoadPlugin,
                                          symbol.error()));
    }

    // ABI boundary: this symbol is the documented no-argument C entry returning
    // a PluginEntry pointer. Valid plugins provide readable version/size fields.
    const auto query = reinterpret_cast<PluginQuery>(*symbol);
    const auto* entry = query();
    if (!entry || entry->version != plugin_entry_version || entry->size < sizeof(PluginEntry)) {
        return std::unexpected(Error{ErrorCode::IncompatiblePlugin, ErrorOperation::LoadPlugin});
    }
    if (!entry->module_type || type != entry->module_type || !entry->initialize ||
        !entry->create || !entry->destroy || !entry->clear) {
        return std::unexpected(Error{ErrorCode::InvalidPlugin, ErrorOperation::LoadPlugin});
    }

    auto initialized = entry->initialize(services, *coordination);
    if (!initialized) {
        return std::unexpected(wrap_error(ErrorCode::ServiceFailed, ErrorOperation::InitializePlugin,
                                          initialized.error()));
    }

    rejection.accepted = true;
    return LoadedPlugin(std::move(*library), entry);
}

Result<PluginModule> LoadedPlugin::create() const {
    auto created = entry_->create();
    if (!created) {
        return std::unexpected(created.error());
    }
    if (!*created) {
        return std::unexpected(Error{ErrorCode::InvalidFactory, ErrorOperation::ConstructModule});
    }

    return PluginModule(*created, entry_->destroy);
}
}
