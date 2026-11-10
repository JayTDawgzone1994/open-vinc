#include "../../../../tests/support/manager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 12s;
constexpr auto stop_grace = 2s;
constexpr auto observation_interval = 2ms;

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
    check(ready(), "parallel lifecycle observation timed out");
}
ModuleSnapshot inspect(ModuleManager& manager, MID mid) {
    auto snapshot = manager.inspect(mid);
    check(snapshot.has_value(), "missing Module snapshot");
    return *snapshot;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        path = std::filesystem::temp_directory_path() / ("vinc_parallel_" + std::to_string(std::random_device{}()));
        check(std::filesystem::create_directory(path), "unique lifecycle directory");
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
void run_case(const std::filesystem::path& plugin, const std::string& mode) {
    Directory directory;
    // Manager dies before the owned directory, including on test failure.
    auto manager = take(vinc::test::create_manager());
    ModuleType type{"parallelStopTest", plugin, {}};
    type.policy.max_restarts = 0;
    type.policy.stop_timeout = stop_grace;
    take(manager->register_module_type(type));
    const auto launched_at = control::Clock::now();
    auto mid = take(manager->start_module(type.name, {mode, directory.path.string()}));
    // This short Manual main can already be Finishing before wait_started sees
    // Running. The callback marker proves dispatch began without waiting for
    // normal completion, which itself requires our release gate below.
    until([&] { return std::filesystem::exists(directory.path / "entered"); });
    if (mode != "replace") {
        until([&] { return inspect(*manager, mid).state == ModuleState::Stopping; });
        check(!std::filesystem::exists(directory.path / "stop_entered"), "cleanup preceded callback drain");
    }
    if (mode != "stall") {
        std::ofstream release(directory.path / "release");
        release << "ready\n";
        check(bool(release), "release callback gate");
    }
    until([&] {
        const auto state = inspect(*manager, mid).state;
        return state == ModuleState::Stopped || state == ModuleState::Failed;
    });
    const auto final = inspect(*manager, mid);
    check(final.last_exit.has_value(), "terminal state without OS exit");
    if (mode == "throw") {
        check(final.last_failure && final.last_failure->error.is(module_system::ErrorCode::HookException) &&
              final.last_failure->error.is_operation(module_system::ErrorOperation::SubscriptionCallback),
              "worker fault lost during normal-completion drain");
    } else if (mode == "stall") {
        check(!std::filesystem::exists(directory.path / "stop_entered"), "stalled callback reached cleanup");
        // Either Host's watchdog reports its deadline, or central termination
        // wins that race. Neither route promises a platform-specific exit code.
        const bool reported_deadline = final.cleanup_failure &&
            final.cleanup_failure->error.is(module_system::ErrorCode::StopDeadlineExpired);
        const bool observed_exit = final.last_failure &&
            final.last_failure->error.is(module_system::ErrorCode::ProcessExited);
        check(reported_deadline || observed_exit, "missing supervised exit diagnostic");
        check(final.last_exit->kind != osal::ExitKind::Code || final.last_exit->value != 0,
              "stalled callback falsely completed normally");
        const auto elapsed = control::Clock::now() - launched_at;
        check(elapsed >= stop_grace && elapsed < test_timeout, "stop deadline not enforced");
    } else {
        if (final.last_failure) {
            throw std::runtime_error(error_to_string(final.last_failure->error));
        }
        check(final.state == ModuleState::Stopped && !final.cleanup_failure, "normal drain failed");
        check(std::filesystem::exists(directory.path / "cleaned"), "on_stop did not join application worker");
        check(final.last_exit->kind == osal::ExitKind::Code && final.last_exit->value == 0, "normal process exit");
    }
    manager->begin_shutdown();
    take(manager->wait_shutdown(control::Clock::now() + test_timeout));
    std::cout << mode << " passed\n";
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "lifecycle Plugin argument required");
        for (const auto* mode : {"return", "replace", "throw", "stall"}) {
            run_case(argv[1], mode);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
