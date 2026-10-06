// Compile an application module using only the aggregate public header.
#include <vinc/vinc.hpp>

class SdkModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    vinc::Result<int32_t> module_interface(std::vector<std::string>) override {
        return 17;
    }
};

REGISTER_MODULE(SdkModule, "sdkModule")
