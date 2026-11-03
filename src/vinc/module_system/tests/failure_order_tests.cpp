#include "plugin_test_support.hpp"
#include <vinc/module/detail/module_runtime.hpp>
// Reuse the existing termination seam in a test-only Manager compilation.
// No production source, ABI, deadline, or primary-fault policy is changed.
#include "../src/host/module_manager.cpp"

namespace {
using namespace vinc;
using namespace vinc::test::plugin;
std::atomic<bool> defer_termination{false};
std::atomic<unsigned> termination_attempts{0};
constexpr auto startup_policy_budget = 5s;

struct TerminationGate {
    TerminationGate() {
        termination_attempts = 0;
        defer_termination = true;
    }
    ~TerminationGate() {
        // Declared after Scope: restore real termination before failure cleanup.
        defer_termination = false;
    }
};

ModuleSnapshot await_primary(ModuleManager& manager, MID mid, module_system::ErrorCode code,
                             module_system::ErrorOperation operation) {
    const auto deadline = Clock::now() + wait_budget;
    for (;;) {
        auto snapshot = manager.inspect(mid);
        check(snapshot.has_value(), "order-test record exists");
        if (snapshot->last_failure) {
            const auto& primary = *snapshot->last_failure;
            if (primary.mid != mid || !primary.error.is(code) ||
                primary.error.operation != static_cast<std::int32_t>(operation)) {
                std::cerr << "MID=" << mid << " actual=" << error_to_string(primary.error) << '\n';
                check(false, "central saved expected primary identity");
            }
            check(!snapshot->last_exit, "central primary observed while child still owned");
            return *snapshot;
        }
        check(Clock::now() < deadline, "central primary deadline");
        std::this_thread::sleep_for(poll_interval);
    }
}

void test_order(const std::filesystem::path& host, bool timeout_first) {
    Scope scope;
    scope.create(host);
    TerminationGate termination_gate;
    ModulePolicy policy;
    policy.startup_timeout = startup_policy_budget;
    ModuleType type;
    type.name = "scripted";
    type.library = host;
    type.policy = policy;
    check(bool(scope.manager->register_module_type(std::move(type))), "register scripted peer");
    const auto mid = take(scope.manager->start_module("scripted", {take(osal::path_to_utf8(scope.directory))}));
    wait_file(scope.directory / "connected", Clock::now() + wait_budget);
    const auto code = timeout_first ? module_system::ErrorCode::StartupTimeout : module_system::ErrorCode::HookFailed;
    const auto operation = timeout_first ? module_system::ErrorOperation::Ready : module_system::ErrorOperation::ConstructModule;
    if (!timeout_first) {
        signal_file(scope.directory / "first");
    }

    // This snapshot, not a child marker/send/ack, authorizes the next report.
    const auto saved = await_primary(*scope.manager, mid, code, operation);
    const auto original = saved.last_failure->error;
    if (timeout_first) {
        const auto deadline = Clock::now() + wait_budget;
        while (termination_attempts.load() == 0) {
            check(Clock::now() < deadline, "startup timeout requested termination");
            std::this_thread::sleep_for(poll_interval);
        }
        signal_file(scope.directory / "first");
    }
    wait_file(scope.directory / "first-acknowledged", Clock::now() + wait_budget);
    signal_file(scope.directory / "later");
    wait_file(scope.directory / "later-acknowledged", Clock::now() + wait_budget);
    const auto inspected = scope.manager->inspect(mid);
    check(inspected.has_value(), "record retained after secondary reports");
    const auto& after = *inspected;
    check(after.last_failure && after.last_failure->mid == mid &&
          after.last_failure->error.record() == original.record(), "later primary did not replace central primary");
    check(after.cleanup_failure && after.cleanup_failure->mid == mid &&
          after.cleanup_failure->error.is(module_system::ErrorCode::HookException) &&
          after.cleanup_failure->error.operation == static_cast<std::int32_t>(module_system::ErrorOperation::StopHook),
          "central saved cleanup separately");
    if (!timeout_first) {
        check(after.last_failure->error.causes().size() == 1 &&
              after.last_failure->error.causes().front() ==
                  Error{module_system::ErrorCode::InvalidFactory, module_system::ErrorOperation::ConstructModule}.record(),
              "primary cause preserved after secondary reports");
    }
    signal_file(scope.directory / "exit");
    auto started = scope.manager->wait_running(mid, Clock::now() + wait_budget);
    check(!started && started.error().is(code), "public wait returns retained central primary");
    const auto final = scope.manager->inspect(mid);
    check(final && final->last_exit && final->last_exit->kind == osal::ExitKind::Code &&
          final->last_exit->value == detail::module_application_failure_exit,
          "scripted child actually exits after acknowledged reports");
    defer_termination = false;
    check(scope.finish(), "order-test child reclaimed before files removed");
    std::cout << (timeout_first ? "timeout saved first" : "Plugin error saved first") << " passed\n";
}
}

namespace vinc::module_system::test {
void checkpoint(Point, osal::Process*, std::string_view) {}

Result<osal::TerminateResult> terminate_process(osal::Process& child) {
    if (defer_termination.load()) {
        ++termination_attempts;
        // Hold only the native kill side effect so the real supervisor can
        // consume a late report after it has saved StartupTimeout. Never block
        // its record lock; the guard restores real kills on every unwind path.
        return osal::TerminateResult::Requested;
    }
    return child.terminate();
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 2, "scripted Host path required");
        const auto host = std::filesystem::absolute(argv[1]);
        test_order(host, false);
        test_order(host, true);
        check(cleanup_failures == 0, "all order-test scopes cleaned");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
