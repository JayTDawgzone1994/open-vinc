#include "../../../../../tests/support/manager.hpp"
#include <vinc/module/host/python_modules.hpp>
#include <vinc/osal/path.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 15s;
constexpr auto stop_grace = 3s;
constexpr auto observation_interval = 5ms;

void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

template<class T>
auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

template<class Predicate>
void until(Predicate ready) {
    const auto deadline = control::Clock::now() + test_timeout;
    while (!ready() && control::Clock::now() < deadline) {
        std::this_thread::sleep_for(observation_interval);
    }
    check(ready(), "Python parallel test observation timed out");
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
            ("vinc_py_parallel_" + std::to_string(std::random_device{}()));
        check(std::filesystem::create_directory(path), "unique Python test directory");
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct OutputOnExit {
    output_hub::Subscription& output;
    ~OutputOnExit() {
        for (const auto& event : output.take().events) {
            std::cout << event->bytes;
        }
    }
};

void run(const std::filesystem::path& python, const std::filesystem::path& bootstrap,
         const std::filesystem::path& fixtures, const std::filesystem::path& peer,
         const std::string& entry, const std::string& mode) {
    std::cout << "Python parallel " << entry << '/' << mode << std::endl;
    Directory directory;
    // Directory cleanup follows actual Manager shutdown, also on assertion unwind.
    auto manager = take(test::create_manager());
    auto output = take(manager->output_hub().subscribe());
    OutputOnExit diagnostics{output};
    take(detail::ManagerAccess::configure_params(*manager, directory.path));
    // The native echo uses defaults. Retain the entire finite fixture burst;
    // overwrite loss is a separate Bus contract, not a scheduler-order assertion.
    take(detail::ManagerAccess::param_command(*manager, {"set", "vinc.pub_default_slots", "16"}));
    take(manager->register_module_type(ModuleType{"nativePeer", peer, {}}));
    auto native_mid = take(manager->start_module("nativePeer", {}));
    take(manager->wait_started(native_mid));

    ModuleType type;
    type.name = "parallelPython";
    type.python = PythonLaunch{fixtures / ("parallel_" + entry + ".py"), bootstrap, python};
    type.policy.max_restarts = 0;
    type.policy.stop_timeout = stop_grace;
    take(manager->register_module_type(type));
    auto mid = take(manager->start_module(type.name,
        {"--name", "pythonNode", mode, take(osal::path_to_utf8(directory.path))}));

    auto snapshot = [&] {
        auto record = manager->inspect(mid);
        check(record.has_value(), "missing Python Module");
        return *record;
    };
    auto fail_early = [&] {
        const auto record = snapshot();
        if (record.last_failure) {
            for (const auto& event : output.take().events) {
                std::cerr << event->bytes;
            }
            throw std::runtime_error(error_to_string(record.last_failure->error));
        }
    };
    if (mode == "normal" || mode == "idle") {
        take(manager->wait_started(mid));
        if (mode == "normal") {
            until([&] {
                fail_early();
                return take(manager->command(mid, {"check"}, control::Clock::now() + test_timeout)) == 42;
            });
        }
        take(manager->stop_module(mid, control::Clock::now() + test_timeout));
    } else if (mode != "invalid") {
        until([&] {
            fail_early();
            return std::filesystem::exists(directory.path / "entered");
        });
        until([&] { return snapshot().state == ModuleState::Stopping; });
        check(!std::filesystem::exists(directory.path / "stop_entered"), "on_stop preceded Python callback");
        if (mode != "stall") {
            std::ofstream release(directory.path / "release");
            release << "ready\n";
            check(bool(release), "release Python callback");
        }
    }
    until([&] {
        const auto state = snapshot().state;
        return state == ModuleState::Stopped || state == ModuleState::Failed;
    });
    const auto final = snapshot();
    for (const auto& event : output.take().events) {
        std::cout << event->bytes;
    }
    if (final.last_failure) {
        std::cout << error_to_string(final.last_failure->error) << '\n';
    }
    check(final.last_exit.has_value(), "Python exit was not observed");
    if (mode == "fault") {
        check(final.last_failure && final.last_failure->error.is(module_system::ErrorCode::HookException) &&
              final.last_failure->error.is_operation(module_system::ErrorOperation::SubscriptionCallback),
              "Python callback fault lost in drain");
    } else if (mode == "invalid") {
        check(final.last_failure && final.last_failure->error.is(module_system::ErrorCode::HookFailed) &&
              final.last_failure->error.is_operation(module_system::ErrorOperation::MainHook),
              "invalid Python worker count was not rejected");
        const auto causes = final.last_failure->error.causes();
        const Error expected{module_system::ErrorCode::InvalidArgument, module_system::ErrorOperation::Pump};
        check(!causes.empty() && causes.front() == expected.record(), "invalid options lost their pump cause");
    } else if (mode == "stall") {
        check(!std::filesystem::exists(directory.path / "stop_entered"), "stalled callback reached cleanup");
        const bool deadline = final.cleanup_failure &&
            final.cleanup_failure->error.is(module_system::ErrorCode::StopDeadlineExpired);
        const bool exited = final.last_failure && final.last_failure->error.is(module_system::ErrorCode::ProcessExited);
        check(deadline || exited, "missing stop deadline diagnostic");
        check(final.last_exit->kind != osal::ExitKind::Code || final.last_exit->value != 0, "false normal exit");
    } else {
        check(final.state == ModuleState::Stopped && !final.last_failure && !final.cleanup_failure,
              "Python normal drain failed");
        check(final.last_exit->kind == osal::ExitKind::Code && final.last_exit->value == 0, "Python exit code");
        check(std::filesystem::exists(directory.path / "cleaned"), "Python on_stop did not finish");
        check(std::filesystem::exists(directory.path / "destroyed"), "Python callback cycles were not released");
    }
    check(manager->inspect(native_mid)->state == ModuleState::Running, "Python affected unrelated native Module");
    manager->begin_shutdown();
    take(manager->wait_shutdown(control::Clock::now() + test_timeout));
}

void examples(const std::filesystem::path& python, const std::filesystem::path& bootstrap,
              const std::filesystem::path& plugin, const std::filesystem::path& script) {
    Directory directory;
    auto manager = take(test::create_manager());
    auto output = take(manager->output_hub().subscribe());
    OutputOnExit diagnostics{output};
    take(detail::ManagerAccess::configure_params(*manager, directory.path));
    take(detail::ManagerAccess::param_command(*manager, {"set", "vinc.pub_default_slots", "16"}));
    take(manager->register_module_type(ModuleType{"parallelExample", plugin, {}}));
    ModuleType type;
    type.name = "parallelPython";
    type.python = PythonLaunch{script, bootstrap, python};
    take(manager->register_module_type(type));
    auto native = take(manager->start_module("parallelExample", {}));
    auto interpreted = take(manager->start_module(type.name, {"--name", "parallelPy"}));
    take(manager->wait_started(native));
    take(manager->wait_started(interpreted));
    take(detail::ManagerAccess::param_command(*manager, {"set", "parallelCpp.gain", "3"}));
    take(detail::ManagerAccess::param_command(*manager, {"set", "parallelPy.gain", "3"}));

    // Parameter updates and Topics may overlap; this checks safe delivery, not
    // an atomic configuration/publication boundary that the SDK never promises.
    for (const auto mid : {native, interpreted}) {
        take(manager->command(mid, {"send"}, control::Clock::now() + test_timeout));
    }
    std::string captured;
    until([&] {
        for (const auto mid : {native, interpreted}) {
            check(take(manager->command(mid, {"status"}, control::Clock::now() + test_timeout)) == 0,
                  "example status failed");
        }
        for (const auto& event : output.take().events) {
            captured += event->bytes;
        }
        return captured.find("parallelCpp received=4") != std::string::npos &&
            captured.find("parallelPy received=4") != std::string::npos;
    });
    std::cout << captured;
    manager->begin_shutdown();
    take(manager->wait_shutdown(control::Clock::now() + test_timeout));
    for (const auto mid : {native, interpreted}) {
        auto final = manager->inspect(mid);
        check(final && final->state == ModuleState::Stopped && !final->last_failure && !final->cleanup_failure,
              "tutorial Module failed during shutdown");
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 7, "python/bootstrap/fixtures/peer/native-example/python-example arguments");
        const auto python = std::filesystem::absolute(argv[1]);
        const auto bootstrap = std::filesystem::absolute(argv[2]);
        const auto fixtures = std::filesystem::absolute(argv[3]);
        const auto peer = std::filesystem::absolute(argv[4]);
        for (const auto* entry : {"framework", "manual"}) {
            run(python, bootstrap, fixtures, peer, entry, "normal");
            run(python, bootstrap, fixtures, peer, entry, "idle");
        }
        for (const auto* mode : {"return", "fault", "stall"}) {
            run(python, bootstrap, fixtures, peer, "manual", mode);
        }
        run(python, bootstrap, fixtures, peer, "invalid", "invalid");
        examples(python, bootstrap, std::filesystem::absolute(argv[5]), std::filesystem::absolute(argv[6]));
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
