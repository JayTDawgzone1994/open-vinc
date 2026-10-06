#include <vinc/osal/path.hpp>
// Test-only BaseModule with controlled startup, command and stop behaviors.
// Intentional hangs/crashes let the parent test the framework's supervision boundaries.

#include <vinc/module/base_module.hpp>
#include <vinc/message/storage.hpp>
#include <atomic>
#include <cstdio>
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

class TestModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
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

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (!args.empty() && args[0] == "contract-start") {
            vinc::contract_violation("startup contract");
        }
        if (!args.empty() && args[0] == "dual-failure") {
            fail_stop_ = true;
            const auto native = vinc::NativeError{static_cast<vinc::NativeErrorSource>(999), -123456};
            return std::unexpected(
                vinc::Error::from_raw(static_cast<vinc::ErrorDomain>(999), -71, -72, native));
        }
        if (!args.empty() && args[0] == "stop-failure") {
            fail_stop_ = true;
        }
        if (args.size() == 2 && args[0] == "start-marker") {
            auto path = vinc::osal::path_from_utf8(args[1]);
            if (!path) {
                return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::InvalidArgument,
                                                        vinc::module_system::ErrorOperation::StartHook,
                                                        path.error()));
            }

            std::ofstream(*path) << "started";
        }
        if (!args.empty() && args[0] == "init-hang") {
            std::this_thread::sleep_for(simulated_hang_duration);
        }
        if (!args.empty() && args[0] == "fail") {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }

        if (!args.empty() && args[0] == "crash-start") {
            std::_Exit(application_crash_exit);
        }
        if (args.size() == 2 && args[0] == "marker") {
            auto path = vinc::osal::path_from_utf8(args[1]);
            if (!path) {
                return std::unexpected(vinc::wrap_error(vinc::module_system::ErrorCode::InvalidArgument,
                                                        vinc::module_system::ErrorOperation::StartHook,
                                                        path.error()));
            }

            stop_marker_path = std::move(*path);
        }
        return {};
    }

    vinc::Result<int32_t> module_interface(std::vector<std::string> args) override {
        if (args.empty()) {
            return normal_command_status;
        }

        if (args[0] == "contract" || args[0] == "contract-stdio" || args[0] == "contract-concurrent") {
            if (args[0] == "contract-stdio") {
                std::atomic<bool> locked{false};
                std::thread holder([&] {
#ifdef _WIN32
                    _lock_file(stderr);
#else
                    flockfile(stderr);
#endif
                    locked.store(true);
                    std::this_thread::sleep_for(simulated_hang_duration);
                });
                holder.detach();
                while (!locked.load()) {
                    std::this_thread::yield();
                }
            }
            vinc::MessageArray<std::string> array;
            auto resized = array.resize(3);
            if (!resized) {
                return std::unexpected(resized.error());
            }
            if (args[0] == "contract-concurrent") {
                constexpr int competing_reporters = 3;
                std::atomic<int> ready{0};
                std::atomic<bool> start{false};
                for (int reporter = 0; reporter < competing_reporters; ++reporter) {
                    std::thread([&] {
                        ready.fetch_add(1);
                        while (!start.load()) {
                            std::this_thread::yield();
                        }
                        (void)std::as_const(array)[10];
                    }).detach();
                }
                while (ready.load() != competing_reporters) {
                    std::this_thread::yield();
                }
                start.store(true);
                (void)std::as_const(array)[10];
            }
            (void)array[10];
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

        if (args[0] == "negative") {
            return -111;
        }

        if (args[0] == "typed-failure") {
            return std::unexpected(vinc::Error::from_raw(static_cast<vinc::ErrorDomain>(999), -81, -82));
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

    vinc::Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) override {
        return std::vector<std::string>{"hello", "status"};
    }

    vinc::Result<void> on_stop() override {
        if (fail_stop_) {
            return std::unexpected(vinc::Error::from_raw(static_cast<vinc::ErrorDomain>(999), -91, -92));
        }
        if (!stop_marker_path.empty()) {
            std::ofstream marker_file(stop_marker_path);
            marker_file << get_module_name();
        }
        return {};
    }

private:
    bool fail_stop_ = false;
    std::filesystem::path stop_marker_path;
};
REGISTER_MODULE(TestModule, "helper")
