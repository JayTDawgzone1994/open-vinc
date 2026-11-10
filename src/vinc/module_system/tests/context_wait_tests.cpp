// Use the actual Host adapter with isolated checkpoints, never a production hook.
#include "../src/runtime/module_runtime.cpp"
#include <future>
#include <random>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 3s;
constexpr auto short_wait = 2ms;
constexpr auto recovery_holdoff = 1s;
std::function<void(module_system::test::Point, std::string_view)> checkpoint_action;

void check(bool condition, const char* reason) {
    if (!condition) {
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

class WaitModule final : public BaseModule<RunMode::Manual> {
public:
    using BaseModule::BaseModule;
    Context& access() {
        return context;
    }
    Result<void> main() override {
        return {};
    }
    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};

// All transport owners outlive the Module; no endpoint or service is Plugin-owned.
struct Fixture {
    std::string name = "wait_" + std::to_string(std::random_device{}());
    HostNotification notification;
    detail::ModuleLocks locks{{}, 1};
    detail::ModuleParams params{{}, locks};
    rpc::detail::Host rpc{nullptr};
    RuntimeInbox inbox;
    std::unique_ptr<WaitModule> module = std::make_unique<WaitModule>("waitNode");
    HostContext host{*module, {}, inbox};

    Fixture() {
        auto creator = take(osal::NotificationReceiver::create(name));
        take(notification.open(name));
        inbox.notification = &notification;
        inbox.params = &params;
        inbox.locks = &locks;
        inbox.rpc = &rpc;
        detail::ModuleMessageBusAccess::attach(*module, {}, host, params, nullptr, rpc);
    }
    ~Fixture() {
        checkpoint_action = {};
        notification.cancel();
        module.reset();
        (void)notification.close();
        (void)osal::NotificationReceiver::unlink_name(name);
    }
    Context& context() {
        return module->access();
    }
};

void test_deadlines_and_hints() {
    WaitModule unbound("unbound");
    auto rejected = unbound.access().waitForWork();
    check(!rejected && rejected.error().is(module_system::ErrorCode::ResourcesUnavailable) &&
          rejected.error().is_operation(module_system::ErrorOperation::WaitForWork), "unbound wait error");

    Fixture fixture;
    std::string outcome;
    checkpoint_action = [&](auto point, auto value) {
        if (point == module_system::test::Point::NotificationWaitFinished) {
            outcome = value;
        }
    };
    fixture.notification.wake();
    take(fixture.context().waitForWork(control::Clock::now()));
    check(outcome.empty(), "expired deadline never enters native wait");
    take(fixture.context().waitForWork());
    check(outcome == "notified", "expired deadline preserved the queued notification");

    outcome.clear();
    const auto deadline = control::Clock::now() + short_wait;
    take(fixture.context().waitForWork(deadline));
    // A descheduled caller may reach admission after its deadline; that path
    // correctly skips the native wait instead of manufacturing a Timeout.
    check((outcome.empty() || outcome == "timeout") && control::Clock::now() >= deadline,
          "caller deadline honored");
    const auto started = control::Clock::now();
    take(fixture.context().waitForWork());
    check(outcome == "timeout" && control::Clock::now() - started >= config::module_pump_idle_interval,
          "default wait retains the fallback scan");

    // Signal at the check-to-sleep boundary, with no later signal to rescue it.
    checkpoint_action = [&](auto point, auto value) {
        if (point == module_system::test::Point::NotificationWait) {
            fixture.notification.wake();
        } else if (point == module_system::test::Point::NotificationWaitFinished) {
            outcome = value;
        }
    };
    outcome.clear();
    take(fixture.context().waitForWork());
    check(outcome == "notified", "boundary wake must use notification, not fallback timeout");
}

void test_busy_and_stop(bool parallel_loop = false) {
    Fixture fixture;
    if (parallel_loop) {
        auto invalid = fixture.context().pumpLoop({.topic_workers = config::module_topic_worker_limit + 1});
        check(!invalid && invalid.error().is(module_system::ErrorCode::InvalidArgument),
              "invalid worker count rejects before starting the loop");
    }
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    checkpoint_action = [&](auto point, auto) {
        if (point == module_system::test::Point::NotificationWait) {
            entered.set_value();
            check(release_future.wait_for(test_timeout) == std::future_status::ready, "test gate release");
        }
    };
    auto waiting = std::async(std::launch::async, [&] {
        if (parallel_loop) {
            return fixture.context().pumpLoop({.topic_workers = 2});
        }
        return fixture.context().waitForWork();
    });
    check(entered_future.wait_for(test_timeout) == std::future_status::ready, "wait reached native boundary");
    for (auto result : {fixture.context().waitForWork(), fixture.context().pumpLoop()}) {
        check(!result && result.error().is(module_system::ErrorCode::Busy), "concurrent loop/wait rejected");
    }
    auto pumped = fixture.context().pumpOnce();
    check(!pumped && pumped.error().is(module_system::ErrorCode::Busy), "concurrent batch rejected");

    // Hold admission while teardown observes it. Stop remains pending even when
    // it races the native wait; no other pump or notification is needed afterward.
    auto quiesced = std::async(std::launch::async, [&] {
        detail::ModuleMessageBusAccess::quiesce(*fixture.module);
    });
    fixture.inbox.stopping = true;
    fixture.notification.cancel();
    release.set_value();
    check(waiting.wait_for(test_timeout) == std::future_status::ready, "stop releases admitted wait");
    take(waiting.get());
    check(quiesced.wait_for(test_timeout) == std::future_status::ready, "teardown observes wait completion");
    quiesced.get();
    check(!fixture.context().living(), "stop is observable");
    check(fixture.notification.wait_diagnostics.failures == 0, "normal cancellation is not a notification fault");
    checkpoint_action = {};
    take(fixture.context().waitForWork());
}

void test_recovery_and_fallback() {
    Fixture fixture;
    take(fixture.notification.receiver.close());
    const auto started = control::Clock::now();
    take(fixture.context().waitForWork());
    check(control::Clock::now() - started >= config::module_pump_idle_interval,
          "failed native wait falls back instead of spinning");
    check(fixture.notification.wait_diagnostics.degraded && fixture.notification.wait_diagnostics.failures == 1,
          "native failure becomes diagnostic, not Module failure");
    fixture.notification.receiver_retry = control::Clock::now() + recovery_holdoff;
    take(fixture.context().waitForWork(control::Clock::now() + short_wait));
    check(fixture.notification.wait_diagnostics.failures == 1, "invalid handle does not busy-retry");
    fixture.notification.receiver_retry = control::Clock::now();
    std::string outcome;
    checkpoint_action = [&](auto point, auto value) {
        if (point == module_system::test::Point::NotificationWait) {
            // A POSIX FIFO need not retain hints after its last reader closes.
            // Recovery must restore future delivery, not invent missing hints.
            fixture.notification.wake();
        } else if (point == module_system::test::Point::NotificationWaitFinished) {
            outcome = value;
        }
    };
    take(fixture.context().waitForWork());
    check(outcome == "notified" && !fixture.notification.wait_diagnostics.degraded &&
          fixture.notification.wait_diagnostics.recoveries == 1, "reopen restores notification delivery");

    // The old fixture path without an endpoint must also honor supplied deadlines.
    fixture.inbox.notification = nullptr;
    const auto fallback_deadline = control::Clock::now() + short_wait;
    take(fixture.context().waitForWork(fallback_deadline));
    check(control::Clock::now() >= fallback_deadline, "condition-variable fallback deadline");
}

// Local snapshot fixture isolates the stop/exception order; real cross-process
// cache reads and mutations are covered by module.parallel.
class ParameterSnapshot final : public param::detail::Access {
public:
    std::string_view module_name() const noexcept override {
        return "waitNode";
    }
    Result<void> register_value(std::string, param::Value, param::RegisterOptions) override {
        return {};
    }
    Result<void> set_value(std::string, param::Value) override {
        return {};
    }
    Result<void> watch_value(std::string, param::Type) override {
        return {};
    }
    Result<std::optional<param::Snapshot>> snapshot(std::uint64_t) override {
        param::Snapshot image;
        image.revision = 1;
        image.entries.emplace("waitNode.value", param::Entry{std::int32_t{1}, std::int32_t{1},
            param::Permission::Limited, 1});
        return std::optional{std::move(image)};
    }
};

void test_param_exception_during_stop() {
    ParameterSnapshot access;
    Fixture fixture;
    param::detail::ClientAccess::bind(fixture.context().param, access);
    take(fixture.context().param.register_param<std::int32_t>("value"));
    take(fixture.context().param.on_change([&](const param::ParamMap&) {
        fixture.inbox.stopping = true;
        throw std::runtime_error("callback failed after stop was requested");
    }));
    auto pumped = fixture.context().pumpOnce();
    check(!pumped && pumped.error().is(module_system::ErrorCode::HookException) &&
          pumped.error().is_operation(module_system::ErrorOperation::ParamCallback),
          "stopping hid a Param callback failure");
    check(fixture.inbox.terminal_failure == std::optional{pumped.error()}, "Host did not latch Param failure");
}

void test_coordinator_allocation_failure() {
    Fixture fixture;
    checkpoint_action = [](auto point, auto) {
        if (point == module_system::test::Point::NotificationWait) {
            throw std::bad_alloc();
        }
    };
    auto result = fixture.context().pumpLoop({.topic_workers = 2});
    check(!result && result.error().is(module_system::ErrorCode::ResourceExhausted),
          "coordinator allocation failure classification");
    check(!fixture.context().living() && fixture.inbox.terminal_failure == std::optional{result.error()},
          "coordinator failure must start supervision before pool teardown");
    check(fixture.inbox.local_stop_deadline != Deadline::max(), "missing failure stop deadline");
    checkpoint_action = {};
    take(fixture.context().waitForWork());
}
}

namespace vinc::module_system::test {
void checkpoint(Point point, osal::Process*, std::string_view detail) {
    if (checkpoint_action) {
        checkpoint_action(point, detail);
    }
}
}

int main() {
    try {
        test_deadlines_and_hints();
        test_busy_and_stop();
        test_busy_and_stop(true);
        test_recovery_and_fallback();
        test_param_exception_during_stop();
        test_coordinator_allocation_failure();
        std::cout << "Context wait contracts passed\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
