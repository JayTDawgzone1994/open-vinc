#include "plugin_test_support.hpp"
#include "control_wakeup_trace.hpp"
#include VINC_CONTROL_COST_SOURCE
#include "usage.hpp"
#include <cstdlib>

namespace {
using namespace vinc;
using test::plugin::check;
using test::plugin::take;
namespace probe = test::control_wakeup;
std::atomic<std::uint64_t> passes{0};
std::atomic<std::uint64_t> waits{0};
constexpr auto operation_budget = std::chrono::seconds(30);
constexpr auto idle_period = std::chrono::seconds(10);

void report(std::ostream& output, const char* phase, probe::Clock::time_point start,
            control_cost::Usage before, std::uint64_t old_passes, std::uint64_t old_waits) {
    const auto end = probe::Clock::now();
    const auto after = control_cost::usage();
    output << phase << ',' << std::chrono::duration<double, std::milli>(end - start).count()
           << ',' << after.cpu_ms - before.cpu_ms << ',' << passes - old_passes
           << ',' << waits - old_waits << ',' << after.rss_bytes << ',' << after.peak_rss_bytes
           << ',' << after.threads << ',' << after.handles_or_fds << '\n';
}
}

namespace vinc::test::control_wakeup {
void frame_event(Point point, control::Role role, std::uint64_t generation, const control::Frame& frame) {
    trace.record(point, role, generation, frame);
}
void attached(const std::shared_ptr<control::Session>&) {}
void before_wait() {}
void after_wait(std::cv_status) {
    ++waits;
}
void pass_started(bool, Deadline) {
    ++passes;
}
void before_rpc_submit(rpc::detail::RpcManager&, std::uint64_t, const control::Frame&) {}
void after_rpc_submit(std::uint64_t, const control::Frame&, const Result<void>&) {}
}

int main(int argc, char** argv) {
    try {
        check(argc == 5, "arguments: Host Plugin count output-directory");
        const int count = std::stoi(argv[3]);
        check(count == 4 || count == 128, "supported controlled workload size");
        const auto directory = take(osal::path_from_utf8(argv[4]));
        std::filesystem::create_directories(directory);
        const auto child_trace = take(osal::path_to_utf8(directory / "child.csv"));
#ifdef _WIN32
        check(_putenv_s("VINC_TEST_CONTROL_TRACE", child_trace.c_str()) == 0, "trace environment");
#else
        check(setenv("VINC_TEST_CONTROL_TRACE", child_trace.c_str(), 1) == 0, "trace environment");
#endif
        test::plugin::Scope scope;
        scope.create(take(osal::path_from_utf8(argv[1])));
        ModuleType type;
        type.name = "controlCost";
        type.library = take(osal::path_from_utf8(argv[2]));
        check(bool(scope.manager->register_module_type(std::move(type))), "register cost Module");
        std::ofstream output(directory / "phases.csv");
        output << "phase,wall_ms,cpu_ms,passes,waits,rss_bytes,peak_rss_bytes,threads,handles_or_fds\n";
        auto before = control_cost::usage();
        auto old_passes = passes.load();
        auto old_waits = waits.load();
        auto start = probe::Clock::now();
        const auto mid = take(scope.manager->start_module("controlCost", {argv[3]}));
        check(bool(scope.manager->wait_running(mid, start + operation_budget)), "Module ready");
        report(output, "startup", start, before, old_passes, old_waits);

        before = control_cost::usage();
        old_passes = passes;
        old_waits = waits;
        start = probe::Clock::now();
        std::mutex idle_mutex;
        std::condition_variable idle;
        std::unique_lock idle_lock(idle_mutex);
        idle.wait_until(idle_lock, start + idle_period, [] { return false; });
        report(output, "idle", start, before, old_passes, old_waits);

        before = control_cost::usage();
        old_passes = passes;
        old_waits = waits;
        start = probe::Clock::now();
        for (int index = 0; index < count; ++index) {
            check(take(scope.manager->command(mid, {"add"}, probe::Clock::now() + operation_budget)) == 0,
                  "callback registration command");
        }
        report(output, "commands", start, before, old_passes, old_waits);
        scope.manager->begin_shutdown();
        check(bool(scope.manager->wait_shutdown(probe::Clock::now() + operation_budget)), "shutdown");
        const auto final = scope.manager->inspect(mid);
        check(final && final->last_exit && final->last_exit->kind == osal::ExitKind::Code &&
              final->last_exit->value == 0 && !final->last_failure, "normal Host exit without fault");
        probe::trace.write(directory / "parent.csv");
        output.close();
        check(bool(output), "phase evidence written");
        check(scope.finish(), "owned processes cleaned");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
