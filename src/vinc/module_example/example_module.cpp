// Minimal process module: implement lifecycle/command hooks and register the factory.
// No Socket, heartbeat or process-management code is required in the application class.

#include <module/base_module.hpp>
#include <iostream>

class ExampleModule final : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;
    int32_t on_start(const std::vector<std::string>&) override {
        std::cout << "[" << get_node_name() << "] ready in its own process\n";
        return 0;
    }
    int32_t node_interface(std::vector<std::string> args) override {
        std::cout << "[" << get_node_name() << "]";
        for (const auto& arg : args) std::cout << ' ' << arg;
        std::cout << std::endl;
        return 0;
    }
    std::vector<std::string> node_autoComplete(std::vector<std::string>) override { return {"status", "hello"}; }
    void on_stop() override { std::cout << "[" << get_node_name() << "] stopping\n"; }
};
REGISTER_MODULE(ExampleModule, "exampleModule")
