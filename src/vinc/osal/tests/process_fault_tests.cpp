// Exercise the real service state machine against a deterministic native boundary.
// No OS process is created by this fixture; native-platform tests remain separate.
#include "../src/process.cpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto test_timeout = 2s;
constexpr auto fake_poll_interval = 5ms;
constexpr auto retry_observation_window = 180ms;
std::atomic<bool> observation_lost{false};
std::atomic<bool> failing{true};
std::atomic<bool> terminated{false};
std::atomic<int> terminate_calls{0};
std::atomic<int> poll_calls{0};
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

namespace vinc::osal::detail {
struct MonitorImpl {};
struct ProcessImpl {};
NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}
NativeMonitor::~NativeMonitor() = default;
Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::make_unique<MonitorImpl>()));
}
void NativeMonitor::wake() noexcept {
}
Result<void> NativeMonitor::wait(bool) noexcept {
    std::this_thread::sleep_for(fake_poll_interval);
    return {};
}
NativeProcess::NativeProcess(std::unique_ptr<ProcessImpl> impl) : impl_(std::move(impl)) {
}
NativeProcess::~NativeProcess() = default;
Result<std::unique_ptr<NativeProcess>> NativeProcess::spawn(const SpawnOptions&, NativeMonitor&,
                                                          const ChildPipe*) {
    return std::unique_ptr<NativeProcess>(new NativeProcess(std::make_unique<ProcessImpl>()));
}
std::uint64_t NativeProcess::pid() const noexcept {
    return 123;
}
bool NativeProcess::event_driven() const noexcept {
    return true;
}
Result<std::optional<ExitStatus>> NativeProcess::poll() noexcept {
    ++poll_calls;
    if (observation_lost) {
        return std::unexpected(Error{ErrorCode::ObservationLost, ErrorOperation::Observe});
    }
    if (failing) {
        return std::unexpected(Error{ErrorCode::SystemError, ErrorOperation::Observe,
                                    {NativeErrorSource::Win32, 123}});
    }
    if (terminated) {
        return std::optional(ExitStatus{ExitKind::Code, 42});
    }
    return std::optional<ExitStatus>{};
}
Result<TerminateResult> NativeProcess::terminate() noexcept {
    ++terminate_calls;
    terminated = true;
    return TerminateResult::Requested;
}
}

int main() {
    using namespace vinc;
    using namespace vinc::osal;
    try {
        auto created = ProcessService::create();
        check(bool(created), "create service");
        auto service = std::move(*created);
        SpawnOptions options;
        options.executable = std::filesystem::absolute("fake-child");
        auto spawned = service.spawn(options);
        check(bool(spawned), "spawn fake child");
        auto child = std::move(*spawned);
        check(!child.try_wait(), "observation failure must surface");
        auto original = child.observation_error();
        check(original.has_value(), "retain first diagnostic");
        const auto observation_start = Clock::now();
        const int initial_polls = poll_calls;
        std::this_thread::sleep_for(retry_observation_window);
        const int background_polls = poll_calls - initial_polls;
        // Allow boundary iterations; derive the bound from elapsed time so a
        // delayed test thread does not mistake normal retrying for a busy loop.
        constexpr int boundary_iterations = 2;
        const auto poll_limit = (Clock::now() - observation_start) /
                                vinc::osal::detail::monitor_failure_retry_interval + boundary_iterations;
        check(background_polls > 0 && background_polls <= poll_limit,
              "failed ready watch retries without spinning");
        check(bool(child.terminate()) && terminate_calls == 1, "observation error must allow native control");
        auto unknown = child.try_wait();
        check(!unknown, "termination accepted is not exit proof");
        auto busy = service.forget(child.id());
        check(!busy && busy.error().is(ErrorCode::Busy), "unconfirmed child cannot be forgotten");
        auto shutdown = service.shutdown_until(Clock::now());
        check(!shutdown && shutdown.error().is(ErrorCode::Timeout), "unconfirmed exit blocks shutdown");
        failing = false;
        auto exited = child.wait_until(Clock::now() + test_timeout);
        check(exited && exited->value == 42, "retry can confirm actual exit");
        check(child.observation_error() == original, "successful retry preserves diagnostic");
        auto repeated = child.try_wait();
        check(repeated && repeated->has_value(), "repeated waits retain exit");
        auto reported = service.shutdown_until(Clock::now() + test_timeout);
        check(!reported && reported.error() == *original, "shutdown retains observation diagnostic");
        check(bool(service.forget(child.id())), "confirmed child can be forgotten");
        check(bool(service.shutdown_until(Clock::now() + test_timeout)), "forgotten history no longer blocks service");

        observation_lost = true;
        auto second = ProcessService::create();
        check(bool(second), "create lost-identity service");
        auto lost = second->spawn(options);
        check(bool(lost), "spawn lost-identity fixture");
        check(!lost->try_wait(), "lost ownership is an error");
        auto denied = lost->terminate();
        check(!denied && denied.error().is_operation(ErrorOperation::Terminate), "unsafe termination denied");
        check(terminate_calls == 1, "lost PID never reaches native terminate");
        auto lost_shutdown = second->shutdown_until(Clock::now() + test_timeout);
        check(!lost_shutdown && lost_shutdown.error().is(ErrorCode::ObservationLost), "lost ownership is never fabricated exit");
        std::cout << "process fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
