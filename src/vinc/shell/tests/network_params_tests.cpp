#include "../../../../tests/support/manager.hpp"
#include <vinc/shell/detail/command_router.hpp>
#include <vinc/shell/detail/network_params.hpp>
#include <fstream>
#include <iostream>

namespace {
using namespace vinc;
using Access = detail::ManagerAccess;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / test::instance_name();
    Directory() {
        std::filesystem::create_directories(path);
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void verify() {
    Directory directory;
    const auto name = test::instance_name();
    auto manager = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*manager, directory.path));
    auto settings = std::make_shared<shell::NetworkParams>(*manager);
    const auto initial = take(settings->configure({}, {}, {}));
    check(initial.ip == "127.0.0.1" && initial.port == 31410, "new defaults");
    auto entries = take(Access::read_params(*manager, {"vinc.shell_ip", "vinc.shell_port"}));
    check(entries.at("vinc.shell_port").restart_hint == param::RestartHint::App, "App restart hint");
    check(entries.at("vinc.shell_ip").permission == param::Permission::Limited, "limited central ownership");

    const auto overridden = take(settings->configure({}, "::1", std::uint16_t{0}));
    check(overridden.ip == "::1" && overridden.port == 0, "IPv6 and ephemeral CLI overrides");
    entries = take(Access::read_params(*manager, {"vinc.shell_ip", "vinc.shell_port"}));
    check(take(entries.at("vinc.shell_port").get<std::uint16_t>()) == 0, "CLI visible in live Param");
    check(std::get<std::uint16_t>(entries.at("vinc.shell_port").default_value) == 31410,
          "CLI does not change declaration default");
    auto invalid_cli = settings->configure({}, "localhost", std::uint16_t{12});
    check(!invalid_cli && invalid_cli.error().is(shell::ErrorCode::InvalidArgument), "CLI numeric IP only");

    shell::CommandRouter local(*manager, false, settings);
    shell::CommandRouter remote(*manager, true, settings);
    const auto corrected = take(local.execute({"param", "set", "vinc.shell_ip", "invalid"}));
    check(corrected.message.find("restored ::1") != std::string::npos, "invalid edit restores last accepted IP");
    take(remote.execute({"param", "set", "vinc.shell_ip", "127.0.0.1"}));
    const auto corrected_again = take(local.execute({"param", "set", "vinc.shell_ip", "other-invalid"}));
    check(corrected_again.message.find("restored 127.0.0.1") != std::string::npos,
          "routers share accepted state");
    check(!remote.execute({"param", "set", "vinc.shell_port", "65536"}), "out of range port rejected");
    check(!remote.execute({"param", "set", "vinc.shell_port", "-1"}), "negative port rejected");
    take(local.execute({"param", "save"}));
    take(remote.execute({"param", "set", "vinc.shell_port", "12345"}));
    check(take(settings->configure({}, {}, {})).port == 12345, "live desired port available on next start");
    settings.reset();
    manager.reset();

    auto restarted = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*restarted, directory.path));
    shell::NetworkParams loaded(*restarted);
    check(take(loaded.configure({}, {}, {})).port == 0, "unsaved edit does not replace saved port");
    // Bypass the consumer only to emulate a user-authored invalid file.
    take(Access::param_command(*restarted, {"set", "vinc.shell_ip", "invalid-file"}));
    take(Access::param_command(*restarted, {"save"}));
    restarted.reset();

    auto repaired = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*repaired, directory.path));
    shell::NetworkParams from_file(*repaired);
    check(take(from_file.configure({}, {}, {})).ip == "127.0.0.1", "invalid file falls back before bind");
    const auto file = directory.path / name / "params.toml";
    std::ifstream input(file);
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    check(saved.find("invalid-file") != std::string::npos, "correction does not implicitly save");
}
}

int main() {
    try {
        verify();
        std::cout << "Shell network Param tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
