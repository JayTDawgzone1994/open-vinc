#include <vinc/module/base_module.hpp>

class ExternalModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    vinc::Result<int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};

REGISTER_MODULE(ExternalModule, "ExternalModule")
