#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/system_module.hpp>
#include <iostream>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto test_timeout = 12s;
constexpr auto poll_interval = 10ms;
constexpr auto retry_delay = 100ms;
constexpr auto rate_interval = 3s;
constexpr auto stop_timeout = 500ms;
constexpr std::size_t attempt_budget = 2;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template<class T>
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

template<class T>
void protected_operation(const Result<T>& result) {
    check(!result && result.error().is(module_system::ErrorCode::SystemModuleProtected),
          "ordinary lifecycle operation must reject a System Module");
}

detail::SystemModuleOptions policy(std::string name = "node") {
    detail::SystemModuleOptions options;
    options.name = std::move(name);
    options.restart_delay = retry_delay;
    options.start_limit_interval = rate_interval;
    options.start_limit_burst = attempt_budget;
    return options;
}

ModuleType module_type(const std::filesystem::path& plugin, std::string name = "helper") {
    ModuleType type{std::move(name), plugin, {}};
    type.policy.stop_timeout = stop_timeout;
    return type;
}

template<class Predicate>
ModuleSnapshot wait_snapshot(ModuleManager& manager, const std::string& name, Predicate accepted) {
    const auto deadline = Clock::now() + test_timeout;
    while (Clock::now() < deadline) {
        const auto snapshot = manager.inspect(name);
        if (snapshot && accepted(*snapshot)) {
            return *snapshot;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    throw std::runtime_error("System Module snapshot deadline: " + name);
}

void shutdown(ModuleManager& manager) {
    manager.begin_shutdown();
    take(manager.wait_shutdown(Clock::now() + test_timeout));
}

void test_window() {
    detail::SystemStartWindow window;
    auto options = policy();
    const Deadline origin{};
    check(window.next_start(origin, options) == origin, "initial start is allowed");
    window.record_start(origin, options);
    check(window.next_start(origin + retry_delay, options) == origin + retry_delay,
          "one remaining attempt");
    window.record_start(origin + retry_delay, options);
    check(window.next_start(origin + retry_delay * 2, options) == origin + rate_interval,
          "initial start counts toward rolling limit");
    check(window.next_start(origin + rate_interval, options) == origin + rate_interval,
          "oldest attempt expires at boundary");
    window.record_start(origin + rate_interval, options);
    check(window.next_start(origin + rate_interval, options) == origin + rate_interval + retry_delay,
          "rolling window retains the other attempt");
    const auto idle = origin + rate_interval * 3;
    check(window.next_start(idle, options) == idle, "idle window permits restart immediately");
}

void test_protection_and_crash(const std::filesystem::path& plugin) {
    auto manager = take(test::create_manager());
    // System composition may deliberately share its own command name.
    take(manager->reserve_command_name("node"));
    take(detail::ManagerAccess::register_system_module(*manager, module_type(plugin), policy()));
    protected_operation(manager->start_module("helper"));
    protected_operation(manager->start_module("helper", {}, {.debug = true}));
    protected_operation(manager->unregister_module_type("helper"));
    check(!manager->register_module_type(module_type(plugin, "node")), "system name reserves type namespace");
    check(!manager->register_micro_program("node", [](auto&) -> Result<int32_t> {
        return 0;
    }),
          "system name reserves micro-program namespace");

    const auto first = take(detail::ManagerAccess::start_system_module(*manager, "helper"));
    take(manager->wait_running(first, Clock::now() + test_timeout));
    protected_operation(detail::ManagerAccess::start_system_module(*manager, "helper"));
    protected_operation(manager->stop_module(first, Clock::now() + test_timeout));
    protected_operation(manager->stop_module("node", Clock::now() + test_timeout));
    protected_operation(manager->forget_module(first));
    check(manager->inspect(first)->kind == ModuleKind::System, "central assigns system identity");

    // The old process exits from its hook without a response. Restart must wait
    // for confirmed OS exit, retain that diagnostic, and allocate a different MID.
    check(!manager->command(first, {"crash"}, Clock::now() + test_timeout), "crash has no successful response");
    const auto restarted = wait_snapshot(*manager, "node", [&](const auto& snapshot) {
        return snapshot.mid != first && snapshot.state == ModuleState::Running;
    });
    check(restarted.last_exit.has_value(), "replacement requires confirmed old process exit");
    check(restarted.last_failure && restarted.last_failure->mid == first, "failure retains old MID");
    check(!manager->stop_module(first, Clock::now() + test_timeout), "stale MID cannot target replacement");
    protected_operation(manager->stop_module(restarted.mid, Clock::now() + test_timeout));

    // Ready did not reset the attempt budget: the second crash is rate limited.
    check(!manager->command(restarted.mid, {"crash"}, Clock::now() + test_timeout), "second crash");
    const auto limited = wait_snapshot(*manager, "node", [](const auto& snapshot) {
        return snapshot.state == ModuleState::RestartWaiting && snapshot.restart_rate_limited;
    });
    check(limited.next_restart.has_value(), "rate limit exposes next eligible restart");
    shutdown(*manager);
    const auto stopped = manager->inspect("node");
    check(stopped && stopped->state == ModuleState::Stopped && !stopped->next_restart,
          "shutdown cancels pending system restart");
}

void test_failures(const std::filesystem::path& plugin, bool spawn_failure) {
    const auto host = spawn_failure ? test::host_path().parent_path() / "missing-system-host.exe" : test::host_path();
    auto manager = take(test::create_manager({}, host));
    take(detail::ManagerAccess::register_system_module(*manager, module_type(plugin), policy()));
    take(detail::ManagerAccess::start_system_module(*manager, "helper", {"fail"}));
    const auto limited = wait_snapshot(*manager, "node", [](const auto& snapshot) {
        return snapshot.state == ModuleState::RestartWaiting && snapshot.restart_rate_limited;
    });
    check(limited.last_failure.has_value(), "startup failure remains diagnosable");
    const auto expected_error = spawn_failure ? module_system::ErrorCode::SpawnFailed : module_system::ErrorCode::HookFailed;
    check(limited.last_failure->error.is(expected_error), "fixture reached its intended failure path");
    check(limited.restarts == attempt_budget, "both startup attempts consume the budget");
    check(limited.next_restart && *limited.next_restart > Clock::now(), "restart eligibility is in future");
    const auto next = wait_snapshot(*manager, "node", [&](const auto& snapshot) {
        return snapshot.mid != limited.mid;
    });
    check(Clock::now() >= *limited.next_restart, "no attempt before rate-window expiry");
    check(next.mid > limited.mid, "failed launch also gets a fresh MID");
    shutdown(*manager);
}

void test_completion(const std::filesystem::path& plugin, config::SystemModuleRestartMode mode,
                     const std::string& action, bool restarts) {
    auto manager = take(test::create_manager());
    auto options = policy("contextNode");
    options.restart_mode = mode;
    take(detail::ManagerAccess::register_system_module(*manager, module_type(plugin, "contextTest"), options));
    const auto first = take(detail::ManagerAccess::start_system_module(*manager, "contextTest", {action}));
    const auto final = wait_snapshot(*manager, "contextNode", [&](const auto& snapshot) {
        return restarts ? snapshot.mid != first : snapshot.state == ModuleState::Stopped;
    });
    check(final.last_exit.has_value(), "completion requires OS exit");
    if (action == "finish") {
        check(!final.last_failure, "normal completion does not invent a fault");
    } else {
        check(final.last_failure.has_value(), "raw exit zero is not protocol completion");
    }
    if (!restarts) {
        check(final.restarts == 0, "OnFailure does not restart successful task");
        protected_operation(manager->forget_module(first));
    }
    shutdown(*manager);
}

void test_invalid_policy(const std::filesystem::path& plugin) {
    auto manager = take(test::create_manager());
    auto invalid = policy();
    invalid.start_limit_burst = 0;
    const auto result = detail::ManagerAccess::register_system_module(*manager, module_type(plugin), invalid);
    check(!result && result.error().is(module_system::ErrorCode::InvalidPolicy), "zero burst rejected");
    invalid.start_limit_burst = config::system_module_start_limit_burst + 1;
    check(!detail::ManagerAccess::register_system_module(*manager, module_type(plugin), invalid),
          "history capacity cannot be exceeded");
}

void test_reserved_instance(const std::filesystem::path& process_plugin,
                            const std::filesystem::path& context_plugin) {
    auto manager = take(test::create_manager());
    take(detail::ManagerAccess::register_system_module(*manager, module_type(process_plugin), policy("contextNode")));
    take(manager->register_module_type(module_type(context_plugin, "contextTest")));
    const auto ordinary = take(manager->start_module("contextTest", {"finish"}));
    const auto started = manager->wait_started(ordinary);
    check(!started && started.error().is(module_system::ErrorCode::NameReserved),
          "system name remains reserved before its process starts");
    shutdown(*manager);
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "process and context plugin paths required");
        test_window();
        test_invalid_policy(argv[1]);
        test_reserved_instance(argv[1], argv[2]);
        test_protection_and_crash(argv[1]);
        test_failures(argv[1], false);
        test_failures(argv[1], true);
        test_completion(argv[2], config::SystemModuleRestartMode::OnFailure, "finish", false);
        test_completion(argv[2], config::SystemModuleRestartMode::Always, "finish", true);
        test_completion(argv[2], config::SystemModuleRestartMode::OnFailure, "exit-zero", true);
        std::cout << "System Module policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
