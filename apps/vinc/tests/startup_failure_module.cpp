#include <vinc/module/base_module.hpp>
#include <vinc/osal/error.hpp>
#ifdef VINC_STARTUP_UI_ERRORS
#include <vinc/ui/error.hpp>
#endif

// Deterministic startup failure without reserving a machine-wide listener port.
class StartupFailureModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    StartupFailureModule() : BaseModule("ui") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        const auto error = vinc::Error{vinc::osal::ErrorCode::PermissionDenied,
                                       vinc::osal::ErrorOperation::OpenFileLock};
#ifdef VINC_STARTUP_UI_ERRORS
        return std::unexpected(vinc::wrap_error(vinc::ui::ErrorCode::BindFailed,
                                               vinc::ui::ErrorOperation::Start, error));
#else
        return std::unexpected(error);
#endif
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};
REGISTER_MODULE(StartupFailureModule, "webUi")
