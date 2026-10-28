// Compile the actual lifecycle dispatcher with test-only fault checkpoints.
#include "../src/runtime/module_runtime.cpp"
#include <stdexcept>
#include <random>
#include <future>

namespace {
int reports = 0;
bool fail_ready = false;
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
class LifecycleFixture final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    LifecycleFixture() : BaseModule("fixture") {}
    bool fail_start = true;
    int starts = 0;
    int stops = 0;
    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        ++starts;
        if (fail_start) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }
        return {};
    }
    vinc::Result<void> on_stop() override {
        ++stops;
        return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                           vinc::module_system::ErrorOperation::StopHook});
    }
};
}
namespace vinc::module_system::test {
void checkpoint(Point point, osal::Process*, std::string_view) {
    if (point == Point::ReportFailure) {
        ++reports;
        throw std::bad_alloc();
    }
    if (point == Point::Ready && fail_ready) {
        throw std::bad_alloc();
    }
}
}
int main() {
    using namespace vinc;
    try {
        const auto name = "host_n" + std::to_string(std::random_device{}());
        auto creator = osal::NotificationReceiver::create(name);
        check(bool(creator), "create Host notification fixture");
        struct NotificationCleanup {
            std::string name;
            ~NotificationCleanup() {
                (void)osal::NotificationReceiver::unlink_name(name);
            }
        } cleanup{name};
        HostNotification notification;
        auto sender = osal::NotificationSender::open(name);
        check(sender && sender->notify(), "queue before Host open");
        check(bool(notification.open(name)), "Host opens existing endpoints");
        check(bool(creator->close()), "close central creator");
        check(bool(notification.receiver.wait_until(control::Clock::now())), "Host open preserves queued hint");
        check(bool(notification.sender.notify()), "Host local sender");
        check(bool(notification.receiver.wait_until(control::Clock::now())), "Host local notification");
        check(bool(notification.receiver.close()), "inject an unusable receiver between waits");
        const auto fallback_start = control::Clock::now();
        notification.wait();
        check(control::Clock::now() - fallback_start >= config::module_pump_idle_interval,
              "failed wait still observes fallback interval instead of spinning");
        check(notification.wait_diagnostics.degraded && notification.wait_diagnostics.failures == 1,
              "receiver failure marked without stopping Module");
        notification.wait();
        check(notification.wait_diagnostics.failures == 1, "wait does not retry a failed handle every pump");
        notification.receiver_retry = control::Clock::now();
        notification.wake();
        notification.wait();
        check(!notification.wait_diagnostics.degraded && notification.wait_diagnostics.recoveries == 1 &&
              notification.wait_diagnostics.last_error.has_value(), "reopen restores waiting and retains cause");
        check(bool(notification.sender.close()), "inject an unusable local sender between signals");
        notification.wake();
        check(notification.send_diagnostics.degraded, "local sender failure is distinct from receiver health");
        notification.wake();
        check(notification.send_diagnostics.failures == 2, "one transient failure permits the next local wake attempt");
        notification.wake();
        check(notification.send_diagnostics.failures == 2, "repeated local sender failures are throttled");
        notification.sender_retry = control::Clock::now();
        notification.progress();
        check(!notification.send_diagnostics.degraded && notification.send_diagnostics.recoveries == 1,
              "local sender reopens existing generation");
        notification.wait();
        auto waiter = std::async(std::launch::async, [&] { notification.wait(); });
        notification.cancel();
        check(waiter.wait_for(std::chrono::seconds{1}) == std::future_status::ready,
              "stop releases or precedes wait without losing cancellation");
        waiter.get();
        check(bool(notification.close()), "Host explicit close");
        check(!notification.receiver && !notification.sender, "Host endpoints released");
        HostNotification missing;
        auto absent = missing.open("absent_" + std::to_string(std::random_device{}()));
        check(!absent && absent.error().is(module_system::ErrorCode::NotificationFailed),
              "Host cannot create a missing notification");
        check(!missing.receiver && !missing.sender, "failed open retained partial ownership");
        std::vector<std::string> obsolete(detail::bootstrap::argument_count);
        obsolete[detail::bootstrap::marker] = "--vinc-module-v14";
        check(!parse_bootstrap(obsolete), "old bootstrap accepted");
        detail::ModuleLocks locks({}, 1);
        detail::ModuleParams params({}, locks);
        RuntimeInbox inbox;
        inbox.params = &params;
        inbox.locks = &locks;
        std::vector<std::string> args;
        LifecycleFixture rejected;
        auto result = run_started_hooks(rejected, args, {}, inbox);
        check(!result && result.error().is(module_system::ErrorCode::HookFailed), "preserve primary hook failure");
        check(rejected.starts == 1 && rejected.stops == 1, "report failure cannot bypass stop hook");
        check(reports == 2, "primary and cleanup report attempts remain separate");
        LifecycleFixture started;
        started.fail_start = false;
        fail_ready = true;
        auto allocation = run_started_hooks(started, args, {}, inbox);
        check(!allocation && allocation.error().is(module_system::ErrorCode::ResourceExhausted), "framework allocation failure retained");
        check(started.starts == 1 && started.stops == 1, "framework failure still stops exactly once");
        check(reports == 4, "both reports attempted after framework failure");
        std::cout << "runtime fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
