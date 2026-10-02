// Minimal process module: implement lifecycle/command hooks and register the factory.
// No Socket, heartbeat or process-management code is required in the application class.

#include <module/base_module.hpp>
#include <iostream>
#include <stdexcept>

class ExampleModule final : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;
    // This example chooses its own CLI convention; the framework does not parse --name.
    std::string on_name(std::vector<std::string>& args) override {
        for (auto it = args.begin(); it != args.end(); ++it) if (*it == "--name") {
            if (it + 1 == args.end()) throw std::invalid_argument("--name requires a value");
            auto name = *(it + 1); args.erase(it, it + 2); return name;
        }
        return get_module_name();
    }
    int32_t on_start(const std::vector<std::string>&) override {
        std::cout << "[" << get_module_name() << "] ready in its own process\n";
        return 0;
    }
    int32_t module_interface(std::vector<std::string> args) override {
        std::cout << "[" << get_module_name() << "]";
        for (const auto& arg : args) std::cout << ' ' << arg;
        std::cout << std::endl;
        return 0;
    }
    std::vector<std::string> module_autoComplete(std::vector<std::string>) override { return {"status", "hello"}; }
    void on_stop() override { std::cout << "[" << get_module_name() << "] stopping\n"; }
};
REGISTER_MODULE(ExampleModule, "exampleModule")
