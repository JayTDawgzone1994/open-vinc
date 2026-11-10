#include "plugin_test_support.hpp"
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/message_bus/error.hpp>
#include <charconv>
#include <iterator>

namespace {
using namespace vinc;
using namespace vinc::test::plugin;
constexpr auto controlled_stop_budget = 5s;
constexpr auto natural_stop_budget = 700ms;
constexpr int shutdown_race_exit = 72;

ModuleSnapshot await_exit(ModuleManager& manager, MID mid) {
    const auto deadline = Clock::now() + wait_budget;
    for (;;) {
        auto snapshot = manager.inspect(mid);
        check(snapshot.has_value(), "requested generation remains inspectable");
        if (snapshot->last_exit &&
            (snapshot->state == ModuleState::Stopped || snapshot->state == ModuleState::Failed)) {
            return *snapshot;
        }
        check(Clock::now() < deadline, "shutdown probe exit deadline");
        std::this_thread::sleep_for(poll_interval);
    }
}

bool run_case(const std::filesystem::path& host, const std::filesystem::path& plugin,
              std::string scenario, bool observe_only = false) {
    Scope scope;
    scope.create(host);
    const auto log_path = scope.directory / "shutdown.log";
    ModuleType type;
    type.name = "contextTest";
    type.library = plugin;
    type.policy.max_restarts = 0;
    type.policy.stop_timeout = scenario == "natural" ? natural_stop_budget : controlled_stop_budget;
    const auto registered = scope.manager->register_module_type(type);
    if (!registered) {
        throw std::runtime_error(error_to_string(registered.error()));
    }
    std::string mode = "finish";
    if (scenario == "natural") {
        mode = "loss";
    } else if (scenario == "hook-error") {
        mode = "error";
    }
    const auto mid = take(scope.manager->start_module(type.name,
        {mode, take(osal::path_to_utf8(log_path)), scenario}));
    const auto snapshot = await_exit(*scope.manager, mid);
    std::ifstream input(log_path);
    const std::string log{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::cout << "scenario=" << scenario << " MID=" << mid
              << " exit=" << snapshot.last_exit->value << " restarts=" << snapshot.restarts << '\n'
              << log;
    if (snapshot.last_failure) {
        std::cout << "central_primary=" << error_to_string(snapshot.last_failure->error) << '\n';
    }
    if (snapshot.cleanup_failure) {
        std::cout << "central_cleanup=" << error_to_string(snapshot.cleanup_failure->error) << '\n';
    }
    std::cout.flush();
    const auto contains = [&](std::string_view text) {
        return log.find(text) != std::string::npos;
    };
    check(contains("mid=" + std::to_string(mid) + "\n"), "Host diagnostic belongs to requested MID");
    check(!contains("gate_timeout="), "probe gate timed out; not evidence of production defect");
    check(snapshot.last_exit->kind == osal::ExitKind::Code, "expected a real process exit code");
    check(snapshot.restarts == 0, "probe preserves first generation");

    const bool race = snapshot.last_exit->value == shutdown_race_exit;
    if (observe_only) {
        check(snapshot.last_exit->value == 0 || race, "unexpected natural-probe exit; inspect full trace");
        check(scope.finish(), "natural probe cleanup");
        return race;
    }
    const bool malformed = scenario == "malformed";
    check(contains(malformed ? "first_close=Other" : "first_close=FinalTeardown"),
          "first Bus close source matches the scenario");
    const auto first_close = log.find("first_close=");
    check(log.find("first_close=", first_close + 1) == std::string::npos, "first close is recorded once");
    if (malformed) {
        check(contains("hooks_result=ok") && contains("pre_teardown_result=ok"),
              "hooks/destruction/completion exchange succeeded before teardown");
        check(contains("bus_receive closed=0 injected=1 valid_empty_route=0"),
              "malformed notification reaches open Bus and is not silently discarded");
        check(contains("watchdog_bus_protocol=1") && contains("watchdog_exit=72"),
              "exact watchdog classification and cause chain required");
        check(race && snapshot.last_failure, "central retains failure and actual exit 72");
        const auto& error = snapshot.last_failure->error;
        const auto bus_cause = Error{message_bus::ErrorCode::Protocol,
                                     message_bus::ErrorOperation::Receive}.record();
        const bool reported_bus_failure = error.is(module_system::ErrorCode::BusFailed) &&
            error.is_operation(module_system::ErrorOperation::Receive) &&
            error.causes().size() == 1 && error.causes().front() == bus_cause;
        const bool observed_exit = error.is(module_system::ErrorCode::ProcessExited) &&
            error.is_operation(module_system::ErrorOperation::ObserveExit);
        // Session close races best-effort fault delivery. Both precise outcomes
        // are valid; a timeout or unrelated failure is never accepted.
        check(reported_bus_failure || observed_exit, "central retains Bus report or real abnormal exit");
        check(!snapshot.cleanup_failure, "no earlier cleanup failure");
    } else if (scenario == "before-close" || scenario == "during-teardown") {
        check(contains("pre_teardown_result=ok") && contains("bus_result=ok") &&
              contains("bus_receive closed=0 injected=1 valid_empty_route=1"),
              "same notification is accepted before teardown");
        check(snapshot.last_exit->value == 0 && !snapshot.last_failure && !snapshot.cleanup_failure,
              "control case completes normally without a fault");
    } else {
        check(!contains("pre_teardown_result=ok") && contains("pre_teardown_result=ModuleSystem"),
              "diagnostic distinguishes failed hooks from successful completion");
        check(snapshot.last_exit->value == detail::module_application_failure_exit &&
              snapshot.last_failure && snapshot.last_failure->error.is(module_system::ErrorCode::HookFailed),
              "intentional hook failure preserves original classification");
    }
    if (!malformed) {
        check(contains("receiver_exited_before_bus_shutdown=1"), "receiver must exit before retiring Bus");
        check(log.find("watchdog=exited") < first_close, "receiver exit precedes first Bus close");
    }
    check(scope.finish(), "child reaped and probe files removed");
    return race;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 3 || argc == 4 || argc == 5,
              "shutdown Host, Context Plugin, optional repetition count or --case NAME");
        const auto host = std::filesystem::absolute(argv[1]);
        const auto plugin = std::filesystem::absolute(argv[2]);
        if (argc == 5) {
            const auto option = take(osal::path_to_utf8(std::filesystem::path(argv[3])));
            const auto selected = take(osal::path_to_utf8(std::filesystem::path(argv[4])));
            check(option == "--case" &&
                  (selected == "before-close" || selected == "during-teardown" ||
                   selected == "malformed" || selected == "hook-error"), "invalid controlled case");
            run_case(host, plugin, selected);
        } else if (argc == 4) {
            const auto text = take(osal::path_to_utf8(std::filesystem::path(argv[3])));
            unsigned count = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
            constexpr unsigned maximum_repetitions = 10000;
            check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                  count > 0 && count <= maximum_repetitions, "invalid repetition count");
            unsigned failures = 0;
            for (unsigned index = 0; index < count; ++index) {
                if (run_case(host, plugin, "natural", true)) {
                    ++failures;
                }
            }
            std::cout << "natural observations=" << count << " exit72=" << failures << '\n';
        } else {
            run_case(host, plugin, "before-close");
            run_case(host, plugin, "during-teardown");
            run_case(host, plugin, "malformed");
            run_case(host, plugin, "hook-error");
            std::cout << "Receiver lifetime, valid notification and genuine failure contracts passed.\n";
        }
        return cleanup_failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Shutdown evidence test failed: " << error.what() << '\n';
        return 1;
    }
}
