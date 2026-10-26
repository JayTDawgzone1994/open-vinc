// Exercise real library symbols through only vinc.hpp and the SDK target.
#include <vinc/vinc.hpp>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    using namespace std::chrono_literals;

    constexpr auto command_timeout = 3s;
    constexpr auto shutdown_timeout = 5s;

    if (argc != 2) {
        return 1;
    }

    vinc::Result<void> result;
    if (!result || vinc::error_to_string(vinc::Error{}).empty()) {
        return 2;
    }

    auto manager_owner = vinc::ModuleManager::create();

    if (!manager_owner) {
        std::cerr << vinc::error_to_string(manager_owner.error()) << '\n';
        return 10;
    }

    auto& manager = **manager_owner;
    auto registered =
        manager.register_module_type({"sdkModule", std::filesystem::absolute(argv[1])});
    if (!registered) {
        return 3;
    }

    auto started = manager.start_module("sdkModule");
    if (!started || !manager.wait_started(*started)) {
        return 4;
    }

    auto reply = manager.command(*started, {}, vinc::control::Clock::now() + command_timeout);
    if (!reply || *reply != 17) {
        return 5;
    }

    if (!manager.wait_shutdown(vinc::control::Clock::now() + shutdown_timeout)) {
        return 6;
    }

    std::cout << "SDK aggregate include, link and process-module lifecycle passed\n";
    return 0;
}
