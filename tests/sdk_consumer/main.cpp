#include <vinc/vinc.hpp>
#include <vinc_messages/test.hpp>

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    constexpr auto command_timeout = 3s;
    constexpr auto shutdown_timeout = 5s;

    if (argc != 3) {
        return 3;
    }

    auto message = vinc::make_message<vinc::test>();
    if (!message || vinc::error_to_string(vinc::Error{}).empty()) {
        return 1;
    }

    auto host = vinc::osal::path_from_utf8(argv[2]);
    if (!host) {
        return 10;
    }

    auto manager_owner = vinc::ModuleManager::create({}, *host);
    if (!manager_owner) {
        return 10;
    }

    auto& manager = **manager_owner;
    auto path = vinc::osal::path_from_utf8(argv[1]);
    if (!path) {
        return 4;
    }

    auto registered = manager.register_module_type({"ExternalModule", *path});
    if (!registered) {
        return 5;
    }

    auto started = manager.start_module("ExternalModule");
    if (!started || !manager.wait_started(*started)) {
        return 6;
    }

    auto reply = manager.command(*started, {}, std::chrono::steady_clock::now() + command_timeout);
    if (!reply || *reply != 0) {
        return 7;
    }

    if (!manager.wait_shutdown(std::chrono::steady_clock::now() + shutdown_timeout)) {
        return 8;
    }

    return 0;
}
