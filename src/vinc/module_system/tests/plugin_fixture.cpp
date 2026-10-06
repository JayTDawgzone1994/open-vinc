#include <vinc/module/base_module.hpp>
#include <vinc/common_error.hpp>
#include <vinc/osal/path.hpp>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto loader_stall = std::chrono::seconds(60);
#ifdef VINC_PLUGIN_STALL
// Runs before injection. Central startup supervision must remain effective.
const auto stalled = [] {
    std::this_thread::sleep_for(loader_stall);
    return 0;
}();
#endif

class PluginFixture final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    explicit PluginFixture(std::string name) : BaseModule(std::move(name)) {
        if (get_module_name() == "throwConstructor") {
            throw std::runtime_error("fixture constructor failed");
        }
    }

    ~PluginFixture() override {
        if (!marker_.empty()) {
            std::ofstream marker(marker_);
            marker << "destroyed in plugin";
        }
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (!args.empty()) {
            auto path = vinc::osal::path_from_utf8(args.front());
            if (!path) {
                return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
                    vinc::module_system::ErrorOperation::StartHook, path.error()));
            }
            marker_ = std::move(*path);
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 91;
    }

private:
    std::filesystem::path marker_;
};
}

#if defined(VINC_PLUGIN_BAD_VERSION) || defined(VINC_PLUGIN_NULL_FACTORY) || defined(VINC_PLUGIN_REJECT)
extern "C" VINC_PLUGIN_EXPORT const vinc::detail::PluginEntry* vinc_module_plugin() noexcept {
    static const vinc::detail::PluginEntry entry{
#ifdef VINC_PLUGIN_BAD_VERSION
        vinc::detail::plugin_entry_version + 1,
#else
        vinc::detail::plugin_entry_version,
#endif
        sizeof(vinc::detail::PluginEntry), "pluginFixture",
        [](const vinc::detail::RuntimeServices& services) noexcept -> vinc::Result<void> {
#ifdef VINC_PLUGIN_REJECT
            return std::unexpected(vinc::make_error(vinc::common::ErrorCode::InvalidRuntimeServices,
                                                    vinc::common::ErrorOperation::BindRuntimeServices));
#else
            return vinc::detail::bind_runtime_services(services);
#endif
        },
        [](std::string) -> vinc::Result<vinc::detail::ModuleObject*> {
            return nullptr;
        },
        [](vinc::detail::ModuleObject*) noexcept {},
        vinc::detail::clear_runtime_services};
    return &entry;
}
#elif defined(VINC_PLUGIN_NO_ENTRY)
extern "C" VINC_PLUGIN_EXPORT int unrelated_symbol() {
    return 42;
}
#else
REGISTER_MODULE(PluginFixture, "pluginFixture")
#endif
