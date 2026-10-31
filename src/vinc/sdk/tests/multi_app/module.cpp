#include <vinc/vinc.hpp>
#include <vinc_messages/SelectionFrame.hpp>
#include <iostream>
#ifdef STALL_BEFORE_INJECTION
#include <chrono>
#include <thread>
namespace {
// Deliberately exceeds the production startup deadline before Host injection.
constexpr auto stalled_loader_duration = std::chrono::seconds(60);
const auto stalled_loader = [] {
    std::this_thread::sleep_for(stalled_loader_duration);
    return 0;
}();
}
#endif

class Probe final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    Probe() : BaseModule("probe") {
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }

    vinc::Result<void> on_start(const std::vector<std::string>& arguments) override {
        if (arguments.size() != 2) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                vinc::module_system::ErrorOperation::StartHook});
        }

        auto expected = vinc::osal::path_from_utf8(arguments[1]);
        auto actual = vinc::osal::executable_path();
        if (!expected || !actual || *expected != *actual) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                vinc::module_system::ErrorOperation::StartHook});
        }

        auto message = vinc::make_message<vinc::SelectionFrame>();
        message->data().value = 42;
        std::cout << MODULE_KIND << " SELF_HOST_OK " << message->data().value << '\n' << std::flush;
        return {};
    }
};

REGISTER_MODULE(Probe, MODULE_KIND)
