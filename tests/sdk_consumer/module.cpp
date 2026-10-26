#include <vinc/vinc.hpp>
#include <vinc_messages/ExternalFrame.hpp>
#include <iostream>

class ExternalModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    vinc::Result<std::string> on_name(std::vector<std::string>&) override {
        return std::string{"externalNode"};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string>) override {
        auto message = vinc::make_message<vinc::ExternalFrame>();
        message->data().leaf.data().value = 42;
        std::cout << "EXTERNAL_MODULE_SCHEMA=" << vinc::ExternalLeaf::revision << std::endl;
        return 0;
    }
};

REGISTER_MODULE(ExternalModule, "ExternalModule")
