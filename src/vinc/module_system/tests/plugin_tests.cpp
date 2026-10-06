#include <vinc/module/host/module_manager.hpp>
#include <vinc/module/detail/plugin.hpp>
#include <vinc/osal/dynamic_library.hpp>
#include <vinc/osal/path.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 8s;
constexpr auto poll_interval = 10ms;
constexpr auto startup_timeout = 600ms;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void test_library(const std::filesystem::path& path) {
    auto invalid = osal::DynamicLibrary::open("relative.dll");
    check(!invalid && invalid.error().is(osal::ErrorCode::InvalidArgument), "relative library rejected");
    auto library = take(osal::DynamicLibrary::open(path));
    auto missing = library.function("not_an_export");
    check(!missing && missing.error().is(osal::ErrorCode::NotFound), "missing export rejected");
    auto moved = std::move(library);
    check(!library.function(detail::plugin_entry_symbol), "moved owner is empty");
    auto symbol = take(moved.function(detail::plugin_entry_symbol));
    // Test the documented same-build Plugin C export signature, not arbitrary data symbols.
    const auto query = reinterpret_cast<detail::PluginQuery>(symbol);
    check(query()->version == detail::plugin_entry_version, "loaded function is callable");
    check(bool(moved.close()) && bool(moved.close()), "close is idempotent");
    check(!moved.function(detail::plugin_entry_symbol), "closed owner cannot return symbols");
}

void expect_failure(const std::filesystem::path& path, module_system::ErrorCode code,
                    std::string type = "pluginFixture", std::string name = "badPlugin") {
    auto manager = take(ModuleManager::create());
    ModulePolicy policy;
    policy.startup_timeout = startup_timeout;
    check(bool(manager->register_module_type({type, path, name, policy})), "register test plugin");
    const auto mid = take(manager->start_module(type));
    auto started = manager->wait_running(mid, control::Clock::now() + test_timeout);
    check(!started, "bad plugin must not become ready");
    const auto deadline = control::Clock::now() + test_timeout;
    for (;;) {
        auto inspected = manager->inspect(mid);
        check(inspected.has_value(), "failed Module remains inspectable");
        const auto& snapshot = *inspected;
        if (snapshot.last_exit) {
            check(snapshot.last_failure && snapshot.last_failure->error.is(code), "failure category retained");
            return;
        }
        check(control::Clock::now() < deadline, "failed Host is reaped");
        std::this_thread::sleep_for(poll_interval);
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 7, "six plugin fixture paths required");
        const auto valid = std::filesystem::absolute(argv[1]);
        test_library(valid);
        expect_failure(valid.parent_path() / "missing-plugin.dll", module_system::ErrorCode::PluginLoadFailed);
        expect_failure(std::filesystem::absolute(argv[2]), module_system::ErrorCode::InvalidPlugin);
        expect_failure(std::filesystem::absolute(argv[3]), module_system::ErrorCode::IncompatiblePlugin);
        expect_failure(std::filesystem::absolute(argv[4]), module_system::ErrorCode::HookFailed);
        expect_failure(std::filesystem::absolute(argv[5]), module_system::ErrorCode::ServiceFailed);
        expect_failure(std::filesystem::absolute(argv[6]), module_system::ErrorCode::StartupTimeout);
        expect_failure(valid, module_system::ErrorCode::InvalidPlugin, "wrongType");
        expect_failure(valid, module_system::ErrorCode::HookException, "pluginFixture", "throwConstructor");

        auto missing_host = take(ModuleManager::create({}, valid.parent_path() / "missing-host.exe"));
        check(bool(missing_host->register_module_type({"pluginFixture", valid, "missingHost"})),
              "register with missing Host");
        const auto missing_mid = take(missing_host->start_module("pluginFixture"));
        auto missing_start = missing_host->wait_running(missing_mid, control::Clock::now() + test_timeout);
        check(!missing_start && missing_start.error().is(module_system::ErrorCode::SpawnFailed),
              "Host spawn failure differs from Plugin load failure");

        auto manager = take(ModuleManager::create());
        const auto relocated = valid.parent_path() / std::filesystem::path(u8"Plugin 中文 空白.dll");
        std::filesystem::copy_file(valid, relocated, std::filesystem::copy_options::overwrite_existing);
        struct RemoveFixture {
            std::filesystem::path path;
            ~RemoveFixture() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } remove_fixture{relocated};
        check(bool(manager->register_module_type({"pluginFixture", relocated, "workingPlugin"})), "valid registration");
        const auto marker = valid.parent_path() / "plugin-destroyed.txt";
        std::filesystem::remove(marker);
        const auto mid = take(manager->start_module("pluginFixture", {take(osal::path_to_utf8(marker))}));
        check(bool(manager->wait_running(mid, control::Clock::now() + test_timeout)), "valid plugin ready");
        check(take(manager->command(mid, {}, control::Clock::now() + test_timeout)) == 91, "plugin virtual hook");
        check(bool(manager->wait_shutdown(control::Clock::now() + test_timeout)), "Host cleanup completes");
        std::ifstream input(marker);
        std::string contents;
        std::getline(input, contents);
        check(contents == "destroyed in plugin", "Plugin paired destroy ran before exit");
        input.close();
        std::filesystem::remove(marker);
        std::cout << "plugin loader tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
