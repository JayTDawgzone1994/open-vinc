#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/module/detail/plugin_loader.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/module_object.hpp>
#include <vinc/osal/path.hpp>
#include <optional>

namespace vinc {
bool detail::is_module_launch(std::string_view first_argument) noexcept {
    return first_argument.starts_with("--vinc-module");
}

namespace {
class NativeSource final : public detail::ModuleSource {
    std::optional<detail::LoadedPlugin> plugin_;
public:
    Result<void> prepare(const std::vector<std::string>& args,
                         const detail::RuntimeServices& services) override {
        auto path = osal::path_from_utf8(args[detail::bootstrap::plugin_path]);
        if (!path) {
            return std::unexpected(wrap_error(module_system::ErrorCode::PluginLoadFailed,
                module_system::ErrorOperation::LoadPlugin, path.error()));
        }

        auto loaded = detail::LoadedPlugin::load(*path, args[detail::bootstrap::module_type], services);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }

        plugin_.emplace(std::move(*loaded));
        return {};
    }

    Result<detail::PluginModule> create() override {
        return plugin_->create();
    }

    void clear() noexcept override {
        plugin_.reset();
    }
};
}

int run_module_process(const std::vector<std::string>& args) {
    NativeSource source;
    return detail::run_module_runtime(args, source);
}
}
