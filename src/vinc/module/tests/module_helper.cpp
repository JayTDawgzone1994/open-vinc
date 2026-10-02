// Test-only BaseModule with controlled startup, command and stop behaviors.
// Intentional hangs/crashes let the parent test the framework's supervision boundaries.

#include <module/base_module.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#include <cstdlib>
#include <stdexcept>

namespace {
// Long enough for central timeout/termination to win; the test never waits for this sleep.
constexpr auto simulated_hang_duration = std::chrono::minutes(2);
constexpr int naming_crash_exit = 41;
constexpr int application_crash_exit = 42;
constexpr int rejected_start_status = 12;
constexpr int normal_command_status = 42;
}

class TestModule final : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;

    std::string on_name(std::vector<std::string>& args) override {
        if (!args.empty() && args[0] == "name-hang") {
            std::this_thread::sleep_for(simulated_hang_duration);
        }
        if (!args.empty() && args[0] == "name-throw") {
            throw std::runtime_error("name hook failed");
        }

        if (!args.empty() && args[0] == "name-crash") {
            std::_Exit(naming_crash_exit);
        }
        if (args.size() >= 2 && args[0] == "--name") {
            auto name = args[1];
            args.erase(args.begin(), args.begin() + 2);
            return name;
        }

        return get_module_name();
    }

    int32_t on_start(const std::vector<std::string>& args) override {
        if (args.size() == 2 && args[0] == "start-marker") {
            // Test-only UTF-8/path boundary assumes equal-sized char and char8_t code units.
            // TODO: copying through char8_t* over char storage needs an alias-safe replacement (README).
            std::ofstream(std::filesystem::path(
                std::u8string(reinterpret_cast<const char8_t*>(args[1].data()), args[1].size())))
                << "started";
        }
        if (!args.empty() && args[0] == "init-hang") {
            std::this_thread::sleep_for(simulated_hang_duration);
        }
        if (!args.empty() && args[0] == "fail") {
            return rejected_start_status;
        }

        if (!args.empty() && args[0] == "crash-start") {
            std::_Exit(application_crash_exit);
        }
        if (args.size() == 2 && args[0] == "marker") {
            // Same test-only UTF-8 layout assumption and aliasing TODO as start-marker above.
            stop_marker_path = std::filesystem::path(
                std::u8string(reinterpret_cast<const char8_t*>(args[1].data()), args[1].size()));
        }
        return 0;
    }

    int32_t module_interface(std::vector<std::string> args) override {
        if (args.empty()) {
            return normal_command_status;
        }

        if (args[0] == "crash") {
            std::_Exit(application_crash_exit);
        }
        if (args[0] == "hang") {
            std::this_thread::sleep_for(simulated_hang_duration);
        }
        if (args[0] == "throw") {
            throw std::runtime_error("test hook");
        }

        if (args[0] == "name") {
            return args.size() == 2 && args[1] == get_module_name() ? 0 : 1;
        }

        if (args[0] == "mid") {
            return args.size() == 2 && std::stoull(args[1]) == get_mid() ? 0 : 1;
        }

        if (args[0] == "unicode") {
            const bool arguments_match =
                args.size() == 4 && args[1] == "模組測試" && args[2] == "a b\"c" && args[3].empty();
            return arguments_match ? 0 : 1;
        }

        return normal_command_status;
    }

    std::vector<std::string> module_autoComplete(std::vector<std::string>) override {
        return {"hello", "status"};
    }

    void on_stop() override {
        if (!stop_marker_path.empty()) {
            std::ofstream marker_file(stop_marker_path);
            marker_file << get_module_name();
        }
    }

private:
    std::filesystem::path stop_marker_path;
};
REGISTER_MODULE(TestModule, "helper")
