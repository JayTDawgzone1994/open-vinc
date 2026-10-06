// Exercise the real manager, with a test-only termination failure adapter.
#include "../src/host/module_manager.cpp"
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/osal/path.hpp>
#include <iostream>
#include <random>

namespace {
using namespace vinc;
std::filesystem::path fault_host;
using namespace std::chrono_literals;
constexpr auto test_timeout = 12s;
constexpr auto observation_interval = 10ms;
constexpr auto short_stop_timeout = 300ms;
constexpr auto short_startup_timeout = 1500ms;
constexpr auto heartbeat_interval = 50ms;
constexpr auto heartbeat_timeout = 500ms;
constexpr auto heartbeat_observation = 1s;
constexpr auto scheduling_tolerance = 2s;
constexpr auto repeated_observation_step = 1ms;
constexpr auto caller_stop_wait = 1ms;
constexpr auto restart_delay = 100ms;
constexpr std::size_t fixture_bytes = 56 + 256 + 768 + 256;
constexpr std::uint64_t writing_state = 1;
constexpr std::uint64_t empty_state = 0;
constexpr std::uint64_t committed_state = 2;
constexpr std::uint64_t invalid_state = 99;
std::atomic<bool> deny_termination{false};
std::atomic<bool> denied{false};

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

template<class Predicate>
void await(Predicate predicate, const char* message) {
    const auto deadline = control::Clock::now() + test_timeout;
    while (!predicate()) {
        check(control::Clock::now() < deadline, message);
        std::this_thread::sleep_for(observation_interval);
    }
}

void test_deadline_latching() {
    const auto name = "fatal_policy_" + std::to_string(std::random_device{}());
    Record record;
    record.snapshot.mid = 123;
    record.snapshot.state = ModuleState::Running;
    // This fixture tests metadata/deadline policy, not native process observation.
    record.process.emplace();
    record.contract_report = take(detail::ContractReportStorage::create(name, record.snapshot.mid));
    auto memory = take(osal::SharedMemory::open(name, osal::SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(fixture_bytes, osal::SharedMemoryAccess::ReadWrite));
    auto bytes = take(mapping.writable_bytes());
    // The creator constructed SharedWord at page-aligned offset zero; all shared
    // accesses use the OSAL atomic API. The rest is deliberately invalid payload.
    auto& state = *reinterpret_cast<osal::SharedWord*>(bytes.data());
    std::fill(bytes.begin() + sizeof(osal::SharedWord), bytes.end(), std::byte{0xff});
    const auto started = control::Clock::now();
    observe_contract_failure(record, started);
    check(!record.fatal_deadline, "empty state leaves normal lifecycle alone");

    osal::shared_store(state, writing_state);
    observe_contract_failure(record, started);
    const auto deadline = started + config::module_fatal_exit_timeout;
    check(record.stop_deadline == deadline && record.snapshot.state == ModuleState::Stopping,
          "first fatal observation establishes a fixed grace");
    for (const auto next : {empty_state, committed_state, writing_state, invalid_state}) {
        osal::shared_store(state, next);
        observe_contract_failure(record, started + repeated_observation_step);
        record_failure(record, Error{module_system::ErrorCode::ChannelFailed,
                                    module_system::ErrorOperation::Receive});
        check(record.stop_deadline == deadline, "state changes and secondary errors cannot move grace");
    }
    check(record.snapshot.last_failure->error.is(module_system::ErrorCode::ContractViolation),
          "secondary failure does not replace primary fatal observation");
    check(record.snapshot.cleanup_failure.has_value(), "secondary failure retained separately");

    record.module_type.policy.stop_timeout = short_stop_timeout;
    request_user_stop(record);
    const auto earlier_stop = record.stop_deadline;
    request_user_stop(record);
    check(record.stop_deadline == earlier_stop && earlier_stop < deadline,
          "explicit stop shortens grace once, repeated stop never extends it");

    record.fatal_deadline.reset();
    record.user_stop_deadline.reset();
    record.user_stop = false;
    record.snapshot.state = ModuleState::Starting;
    record.startup_deadline = started + short_startup_timeout;
    osal::shared_store(state, writing_state);
    observe_contract_failure(record, started);
    check(record.stop_deadline == record.startup_deadline, "startup deadline wins over longer fatal grace");
    record.fatal_deadline.reset();
    record.snapshot.debug = true;
    record.snapshot.state = ModuleState::Running;
    observe_contract_failure(record, started);
    check(record.stop_deadline == (Deadline::max)(), "debugging suspends automatic fatal termination");
    record_failure(record, Error{module_system::ErrorCode::ChannelFailed,
                                module_system::ErrorOperation::Receive});
    check(record.stop_deadline == (Deadline::max)(), "secondary errors cannot kill paused fatal reporter");
    request_user_stop(record);
    check(record.stop_deadline < (Deadline::max)(), "explicit stop still bounds debug fatal shutdown");
    check(bool(mapping.unmap()) && bool(memory.close()), "fixture mapping released");
}

ModuleType type_for(const std::filesystem::path& helper) {
    ModuleType type;
    type.name = "contractFault";
    type.module_base_name = "fatal-worker";
    type.library = helper;
    type.policy.heartbeat_interval = heartbeat_interval;
    type.policy.heartbeat_timeout = heartbeat_timeout;
    type.policy.startup_timeout = test_timeout;
    type.policy.stop_timeout = short_stop_timeout;
    return type;
}

MID start(ModuleManager& manager, const std::string& name, const std::string& mode,
          const std::string& marker) {
    const auto mid = take(manager.start_module("contractFault", {name, mode, marker}));
    check(bool(manager.wait_running(mid, control::Clock::now() + test_timeout)), "helper reaches Running");
    return mid;
}

void enter_fatal(ModuleManager& manager, MID mid) {
    auto result = manager.command(mid, {"fatal"}, control::Clock::now() + test_timeout);
    check(!result && result.error().is(module_system::ErrorCode::SessionEndedOutcomeUnknown),
          "fatal wakes command waiter with unknown execution outcome");
    const auto snapshot = manager.inspect(mid);
    check(snapshot && snapshot->state == ModuleState::Stopping, "fatal becomes Stopping before OS exit");
    auto next = manager.command(mid, {}, control::Clock::now() + test_timeout);
    check(!next && next.error().is(module_system::ErrorCode::NotRunning), "new commands refused after fatal");
}

ModuleContractDiagnostic await_failure(ModuleManager& manager, MID mid) {
    await([&] { return manager.inspect(mid)->state == ModuleState::Failed; }, "fatal process exits");
    const auto snapshot = *manager.inspect(mid);
    check(snapshot.last_exit.has_value(), "OS exit is confirmed before completion");
    check(snapshot.last_contract_failure.has_value(), "diagnostic survives confirmed exit");
    return *snapshot.last_contract_failure;
}

void test_processes(const std::filesystem::path& helper) {
    auto manager = take(ModuleManager::create({}, fault_host));
    check(bool(manager->register_module_type(type_for(helper))), "register fault fixture");
    const auto marker_path = std::filesystem::temp_directory_path() /
                             ("fatal_stop_" + std::to_string(std::random_device{}()));
    const auto marker = take(osal::path_to_utf8(marker_path));
    const auto survivor = start(*manager, "survivor", "delayed", "");

    const auto delayed = start(*manager, "delayed", "delayed", marker);
    enter_fatal(*manager, delayed);
    auto diagnostic = await_failure(*manager, delayed);
    check(diagnostic.report && !diagnostic.exit_deadline_expired && !diagnostic.termination_requested,
          "writer that completes within grace exits normally with its report");

    for (const auto* mode : {"writing", "committed"}) {
        const auto mid = start(*manager, mode, mode, marker);
        const auto started = control::Clock::now();
        enter_fatal(*manager, mid);
        // Only the application/reporting thread is stuck. Runtime I/O keeps
        // exchanging real heartbeats for longer than its configured timeout.
        std::this_thread::sleep_for(heartbeat_observation);
        auto snapshot = *manager->inspect(mid);
        check(snapshot.state == ModuleState::Stopping && !snapshot.last_exit && !snapshot.cleanup_failure,
              "fatal is supervised independently while the control channel stays healthy");
        check(take(manager->command(survivor, {}, control::Clock::now() + test_timeout)) == 42,
              "another module remains usable while fatal writer waits");
        diagnostic = await_failure(*manager, mid);
        check(control::Clock::now() - started >= config::module_fatal_exit_timeout,
              "no early kill from ordinary stop timeout");
        check(diagnostic.exit_deadline_expired && diagnostic.termination_requested,
              "central terminates stalled writer after grace");
        if (std::string_view(mode) == "writing") {
            check(!diagnostic.report && diagnostic.report.error().is(module_system::ErrorCode::IncompleteFaultReport),
                  "uncommitted bytes are never presented as a valid report");
        } else {
            check(diagnostic.report && diagnostic.report->index == 10 && diagnostic.report->size == 3,
                  "committed report survives a writer stuck before process exit");
        }
    }

    const auto reentrant = start(*manager, "reentrant", "reentrant", marker);
    enter_fatal(*manager, reentrant);
    deny_termination = true;
    const auto stopped_at = control::Clock::now();
    auto stop = manager->stop_module(reentrant, stopped_at + caller_stop_wait);
    check(!stop && stop.error().is(module_system::ErrorCode::Timeout), "caller timeout does not cancel stop");
    await([&] { return denied.load(); }, "injected OS termination denial reached");
    auto snapshot = *manager->inspect(reentrant);
    check(snapshot.state == ModuleState::Stopping && !snapshot.last_exit && snapshot.cleanup_failure &&
              snapshot.cleanup_failure->error.is(module_system::ErrorCode::TerminateFailed),
          "denied OS termination preserves process ownership and cleanup error");
    check(!snapshot.last_contract_failure, "payload is not collected before confirmed exit");
    await([&] { return manager->inspect(reentrant)->state == ModuleState::Stopped; }, "termination retry succeeds");
    snapshot = *manager->inspect(reentrant);
    check(snapshot.last_contract_failure && snapshot.last_contract_failure->termination_requested &&
              snapshot.last_contract_failure->exit_deadline_expired,
          "retry and explicit stop leave truthful final diagnostics");
    check(control::Clock::now() - stopped_at < config::module_fatal_exit_timeout,
          "explicit stop takes priority over fatal grace");
    check(!std::filesystem::exists(marker_path), "fatal supervision never requests user stop hooks");

    const auto shutdown_target = start(*manager, "shutdown-target", "writing", marker);
    enter_fatal(*manager, shutdown_target);
    const auto shutdown_at = control::Clock::now();
    manager->begin_shutdown();
    check(bool(manager->wait_shutdown(control::Clock::now() + test_timeout)), "all processes and bus leases drained");
    snapshot = *manager->inspect(shutdown_target);
    check(snapshot.state == ModuleState::Stopped && snapshot.last_exit && snapshot.last_contract_failure &&
              snapshot.last_contract_failure->exit_deadline_expired,
          "framework shutdown applies explicit-stop policy to an active fatal generation");
    check(control::Clock::now() - shutdown_at < config::module_fatal_exit_timeout,
          "shutdown does not wait out the longer fatal grace");
    check(!std::filesystem::exists(marker_path), "shutdown skips fatal target's user stop hook");

    auto startup_manager = take(ModuleManager::create({}, fault_host));
    auto type = type_for(helper);
    type.policy.startup_timeout = short_startup_timeout;
    check(bool(startup_manager->register_module_type(type)), "register short startup policy");
    const auto startup_at = control::Clock::now();
    const auto startup = take(startup_manager->start_module(type.name, {"startup", "startup", marker}));
    diagnostic = await_failure(*startup_manager, startup);
    check(diagnostic.exit_deadline_expired && !diagnostic.report,
          "fatal before Ready respects startup deadline");
    check(control::Clock::now() - startup_at < short_startup_timeout + scheduling_tolerance,
          "startup fatal does not get a fresh five-second extension");
    startup_manager->begin_shutdown();
    check(bool(startup_manager->wait_shutdown(control::Clock::now() + test_timeout)), "startup fixture cleanup");

    auto restart_manager = take(ModuleManager::create({}, fault_host));
    type = type_for(helper);
    type.policy.max_restarts = 1;
    type.policy.restart_delay = restart_delay;
    check(bool(restart_manager->register_module_type(type)), "register restart policy");
    const auto original_mid = start(*restart_manager, "restart-target", "delayed", "");
    enter_fatal(*restart_manager, original_mid);
    await([&] {
        const auto current = restart_manager->inspect(std::string("restart-target"));
        return current && current->mid != original_mid && current->state == ModuleState::Running;
    }, "restart creates a fresh running generation after a latched fatal");
    snapshot = *restart_manager->inspect(std::string("restart-target"));
    check(snapshot.last_contract_failure && snapshot.last_contract_failure->mid == original_mid,
          "restart retains previous generation's diagnostic");
    check(take(restart_manager->command(snapshot.mid, {}, control::Clock::now() + test_timeout)) == 42,
          "new MID accepts commands without inheriting old fatal state");
    restart_manager->begin_shutdown();
    check(bool(restart_manager->wait_shutdown(control::Clock::now() + test_timeout)), "restart fixture cleanup");
}
}

namespace vinc::module_system::test {
void checkpoint(Point, osal::Process*) {}

Result<osal::TerminateResult> terminate_process(osal::Process& child) {
    if (deny_termination.exchange(false)) {
        denied = true;
        return std::unexpected(Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::Terminate});
    }
    return child.terminate();
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "fault Plugin and Host paths required");
        fault_host = std::filesystem::absolute(argv[2]);
        test_deadline_latching();
        test_processes(std::filesystem::absolute(argv[1]));
        std::cout << "fatal supervision tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
