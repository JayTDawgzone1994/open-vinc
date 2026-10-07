// Minimal process module: implement lifecycle/command hooks and register the factory.
// No Socket, heartbeat or process-management code is required in the application class.

#include <vinc/module/base_module.hpp>
#include <iostream>
#include <stdexcept>

class ExampleModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    // This example chooses its own CLI convention; the framework does not parse --name.
    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        for (auto argument = args.begin(); argument != args.end(); ++argument) {
            if (*argument == "--name") {
                if (argument + 1 == args.end()) {
                    return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                                       vinc::module_system::ErrorOperation::NameHook});
                }

                // Consume only the naming pair; on_start receives the remaining arguments.
                auto name = *(argument + 1);
                args.erase(argument, argument + 2);
                return name;
            }
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        std::cout << "[" << get_module_name() << "] ready in its own process\n";
        return {};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string> args) override {
        std::cout << "[" << get_module_name() << "]";
        for (const auto& argument : args) {
            std::cout << ' ' << argument;
        }

        std::cout << std::endl;
        return 0;
    }

    vinc::Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) override {
        return std::vector<std::string>{"status", "hello"};
    }

    vinc::Result<void> on_stop() override {
        std::cout << "[" << get_module_name() << "] stopping\n";
        return {};
    }
};
REGISTER_MODULE(ExampleModule, "exampleModule")
