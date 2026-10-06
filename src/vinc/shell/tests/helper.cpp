#include <vinc/osal/path.hpp>
// Shell test module with argument/completion probes and controlled blocking hooks.
// Its cleanup marker proves the central shutdown path reached the module lifecycle hook.

#include <vinc/module/base_module.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <cstdlib>

namespace {
// Test workloads: slow commands exceed the short request deadline; hung commands
// outlive shutdown so the manager must terminate the process.
constexpr auto hung_command_delay = std::chrono::minutes(2);
constexpr auto slow_command_delay = std::chrono::milliseconds(350);
constexpr auto slow_stop_delay = std::chrono::milliseconds(500);
}

class ShellHelper : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::filesystem::path directory_;
    std::string mode_;

    void mark(const std::string& suffix) {
        std::ofstream(directory_ / (get_module_name() + suffix)) << "done";
    }

public:
    using BaseModule::BaseModule;

    vinc::Result<std::string> on_name(std::vector<std::string>& arguments) override {
        if (arguments.size() >= 2 && arguments[0] == "--name") {
            auto name = arguments[1];
            arguments.erase(arguments.begin(), arguments.begin() + 2);
            return name;
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>& input) override {
        auto arguments = input;
        if (!arguments.empty() && arguments[0] == "shellHelper") {
            arguments.erase(arguments.begin());
        }

        if (arguments.size() != 2) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }

        mode_ = arguments[0];
        auto directory = vinc::osal::path_from_utf8(arguments[1]);
        if (!directory) {
            return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::InvalidArgument,
                                                    vinc::module_system::ErrorOperation::StartHook,
                                                    directory.error()));
        }

        directory_ = std::move(*directory);
        if (mode_ == "output") {
            std::cout << "startup-output\n" << std::flush;
            std::cerr << "startup-error\n";
        }

        mark(".ready");
        return {};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string> arguments) override {
        if (!arguments.empty() && arguments[0] == get_module_name()) {
            arguments.erase(arguments.begin());
        }

        if (arguments.empty()) {
            return 0;
        }

        if (arguments[0] == "contract") {
            vinc::contract_index_violation("array index out of range", 10, 3);
        }
        if (arguments[0] == "output") {
            std::cout << "command-output\x1b[2J\n" << std::flush;
            std::cerr << "command-error\n";
            mark(".output");
        }

        if (arguments[0] == "crash-output") {
            std::cout << "crash-output\n" << std::flush;
            std::_Exit(42);
        }

        if (arguments[0] == "hang") {
            mark(".working");
            std::this_thread::sleep_for(hung_command_delay);
        }

        if (arguments[0] == "slow") {
            mark(".working");
            std::this_thread::sleep_for(slow_command_delay);
        }

        if (arguments[0] == "fail") {
            mark(".failed-command");
            return 17;
        }

        if (arguments[0] == "check") {
            if (arguments.size() != 4 || arguments[1] != "中文 空白" || !arguments[2].empty() ||
                arguments[3] != "D:\\Some Folder\\a.txt") {
                return 18;
            }

            mark(".checked");
        }

        return 0;
    }

    vinc::Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) override {
        return std::vector<std::string>{"alpha", "beta"};
    }

    vinc::Result<void> on_stop() override {
        if (mode_ == "slow-stop") {
            std::this_thread::sleep_for(slow_stop_delay);
        }

        mark(".stopped");
        if (mode_ == "output") {
            std::cout << "stop-tail-without-newline" << std::flush;
        }
        return {};
    }
};
REGISTER_MODULE(ShellHelper, "shellHelper")
