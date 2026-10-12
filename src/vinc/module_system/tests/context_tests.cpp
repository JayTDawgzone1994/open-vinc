#include "../../../../tests/support/manager.hpp"
#include <vinc/module/host/module_manager.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;
namespace {
constexpr auto test_deadline = 12s;
constexpr auto observation_interval = 10ms;
constexpr auto stop_grace = 700ms;
constexpr auto restart_delay = 100ms;
constexpr auto request_timeout = 2s;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template <class T>
T take(Result<T> result) {
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

void test_case(const std::filesystem::path& plugin, const std::string& mode, bool success, bool stop) {
    auto manager = take(vinc::test::create_manager());
    ModuleType type{"contextTest", plugin, "contextNode", {}};
    type.policy.max_restarts = stop ? 0 : 1;
    type.policy.restart_delay = restart_delay;
    type.policy.stop_timeout = stop_grace;
    take(manager->register_module_type(type));
    auto mid = take(manager->start_module(type.name, {mode}));
    if (stop) {
        take(manager->wait_started(mid));
        if (mode != "callback-stall") {
            check(take(manager->command("contextNode", {}, control::Clock::now() + request_timeout)) == 42,
                  "pump dispatches command on caller thread");
        } else {
            std::this_thread::sleep_for(stop_grace);
        }

        take(manager->stop_module("contextNode", control::Clock::now() + test_deadline));
    }

    const auto deadline = control::Clock::now() + test_deadline;
    std::optional<ModuleSnapshot> final;
    while (control::Clock::now() < deadline) {
        auto snapshots = manager->snapshots();
        if (!snapshots.empty()) {
            final = snapshots.front();
            if (final->state == ModuleState::Stopped || final->state == ModuleState::Failed) {
                break;
            }
        }

        std::this_thread::sleep_for(observation_interval);
    }

    check(final.has_value(), "retain completed record");
    if (success) {
        if (!stop) {
            take(manager->wait_started(mid));
            auto running = manager->wait_running(mid, control::Clock::now() + request_timeout);
            check(!running && running.error().is(module_system::ErrorCode::NotRunning),
                  "finished task proves startup but is not currently running");
        }

        if (final->last_failure) {
            std::cerr << error_to_string(final->last_failure->error) << '\n';
        }

        check(final->state == ModuleState::Stopped && final->restarts == 0, "normal completion must not restart");
        if (!stop) {
            check(final->last_exit && final->last_exit->kind == osal::ExitKind::Code &&
                  final->last_exit->value == 0, "normal completion confirms OS exit");
        }
    } else {
        check(final->state == ModuleState::Failed && final->restarts == 1, "failed generation follows retry budget");
        check(final->last_failure.has_value() || final->cleanup_failure.has_value(), "retain failure diagnostic");
        if (mode == "callback-error") {
            check(final->last_failure && final->last_failure->error.is(module_system::ErrorCode::HookException) &&
                  final->last_failure->error.is_operation(module_system::ErrorOperation::SubscriptionCallback),
                  "callback exception retains its specific diagnostic");
        }
    }

    manager->begin_shutdown();
    take(manager->wait_shutdown(control::Clock::now() + test_deadline));
    std::cout << mode << " passed\n";
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "plugin argument");
        for (const auto* mode : {"finish", "batch", "loss", "cancel", "topology", "new-only"}) {
            test_case(argv[1], mode, true, false);
        }

        for (const auto* mode : {"error", "exit-zero", "cleanup-error", "cleanup-stall", "callback-error"}) {
            test_case(argv[1], mode, false, false);
        }

        for (const auto* mode : {"loop", "worker", "callback-stall"}) {
            test_case(argv[1], mode, true, true);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
