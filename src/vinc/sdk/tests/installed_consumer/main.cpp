// This test also exercises internal Tcl initialization to verify its installed
// static link/resource closure. It is not a user-owned Shell startup example.
#include <vinc/vinc.hpp>
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <vinc_messages/InstalledSample.hpp>
#include <iostream>
#include "local_value.hpp"

int main(int argc, char** argv) {
    // Internal SDK link/cleanup fixture, not a supported custom application main.
    if (argc > 1 && vinc::detail::is_module_launch(argv[1])) {
        return vinc::run_module_process(std::vector<std::string>(argv + 1, argv + argc));
    }

    if (argc != 1) {
        return 1;
    }

    auto message = vinc::make_message<vinc::InstalledSample>();
    if (!message) {
        return 2;
    }
    message->data().sequence = 42;
    auto resized = message->data().bytes.resize(4);
    if (!resized) {
        return 3;
    }
    message->data().bytes[0] = 17;
    if (message->data().bytes[0] != 17 || message->data().sequence != 42) {
        return 4;
    }

    auto initialized = vinc::shell::initialize_tcl(argv[0]);
    if (!initialized) {
        std::cerr << vinc::error_to_string(initialized.error()) << '\n';
        return 5;
    }
    auto commands = vinc::shell::tcl_reserved_commands();
    if (!commands || commands->empty()) {
        return 6;
    }

    auto path = vinc::osal::executable_path();
    if (!path) {
        return 7;
    }
    // This fixture checks Manager cleanup without relying on an installed Host tool.
    auto manager = vinc::ModuleManager::create({}, *path);
    if (!manager) {
        std::cerr << vinc::error_to_string(manager.error()) << '\n';
        return 8;
    }
    constexpr auto shutdown_timeout = std::chrono::seconds(5);
    auto stopped = (*manager)->wait_shutdown(std::chrono::steady_clock::now() + shutdown_timeout);
    if (!stopped) {
        return 9;
    }
    if (vinc::error_to_string(vinc::Error{}).empty()) {
        return 10;
    }
    std::cout << "Installed SDK generation, storage, Tcl and service cleanup passed\n";
    std::cout << "local_value=" << sdk_test_value << '\n';
    return 0;
}
