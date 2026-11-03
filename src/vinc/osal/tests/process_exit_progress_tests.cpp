// Exercise the real service/record state machine. Only the native boundary is fake.
// Non-polling waits have no timer; tests cannot rescue lost progress accidentally.
#include "../src/process.cpp"

#include <deque>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace vinc::osal;
constexpr auto fixture_timeout = 5s;
constexpr auto fallback_interval = 50ms;
constexpr auto idle_window = 130ms;
constexpr auto waiter_deadline = 250ms;
constexpr std::uint64_t reused_pid = 123;
constexpr std::uint32_t exit_code = 42;
constexpr int held_empty_polls = 1000;

struct ChildState {
    ProcessId id;
    bool notified = false;
    bool ready = true;
    bool released = false;
    int empty_remaining = 1;
    int empty_results = 0;
    int polls = 0;
    int exits = 0;
    int signals = 0;
    std::optional<vinc::Error> error;
};

struct Fixture {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ProcessId> events;
    std::vector<std::shared_ptr<ChildState>> children;
    bool wake = false;
    bool waiting = false;
    bool polling = false;
    bool hold_batch = false;
    bool batch_taken = false;
    bool release_batch = false;
    bool hold_spawn = false;
    bool spawn_entered = false;
    bool publish = false;
    bool fail_spawn = false;
    bool monitor_failure = false;
    bool cleanup = false;
    bool monitor_destroyed = false;
    int wake_calls = 0;
    int delivered = 0;
    int batches = 0;
    int timer_returns = 0;
};

Fixture fixture;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Predicate>
void await_fixture(Predicate predicate, const char* message) {
    std::unique_lock lock(fixture.mutex);
    require(fixture.changed.wait_for(lock, fixture_timeout, predicate), message);
}

// No service or record mutex is acquired while holding the fixture mutex.
// Native methods use the opposite direction, so test orchestration must not invert it.
void await_idle() {
    await_fixture([] {
        return fixture.waiting && !fixture.polling && !fixture.wake && fixture.events.empty();
    }, "observer did not return to event-only wait");
}

void require_stays_idle() {
    await_idle();
    std::unique_lock lock(fixture.mutex);
    const int timers = fixture.timer_returns;
    int polls = 0;
    for (const auto& child : fixture.children) {
        polls += child->polls;
    }
    fixture.changed.wait_for(lock, idle_window, [] { return false; });
    int later_polls = 0;
    for (const auto& child : fixture.children) {
        later_polls += child->polls;
    }
    require(fixture.timer_returns == timers && later_polls == polls,
            "observer polls without a remaining progress obligation");
}

struct Cleanup {
    ~Cleanup() {
        std::lock_guard lock(fixture.mutex);
        fixture.cleanup = true;
        fixture.publish = true;
        fixture.release_batch = true;
        fixture.wake = true;
        fixture.changed.notify_all();
    }
};

SpawnOptions options() {
    SpawnOptions result;
    result.executable = std::filesystem::absolute("controlled-exit-child");
    return result;
}

Process spawn(ProcessService& service) {
    auto result = service.spawn(options());
    require(bool(result), "spawn failed");
    return std::move(*result);
}

std::shared_ptr<ChildState> state(ProcessId id) {
    std::lock_guard lock(fixture.mutex);
    for (const auto& child : fixture.children) {
        if (child->id == id) {
            return child;
        }
    }
    throw std::runtime_error("missing fake child");
}

void notify_exit(ProcessId id) {
    std::lock_guard lock(fixture.mutex);
    fixture.events.push_back(id);
    fixture.changed.notify_all();
}

void await_reaped(const std::shared_ptr<ChildState>& child) {
    // Observe fixture state only. A public wait/try_wait here could mask the bug.
    await_fixture([&] { return child->released; }, "background observation did not finish");
}

void require_exit(Process& child) {
    auto result = child.wait_until(Clock::now() + fixture_timeout);
    require(result && result->kind == ExitKind::Code && result->value == exit_code,
            "incorrect retained exit result");
}
}

namespace vinc::osal::detail {
struct MonitorImpl {};
struct ProcessImpl {
    std::shared_ptr<ChildState> child;
};

NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}
NativeMonitor::~NativeMonitor() {
    std::lock_guard lock(fixture.mutex);
    fixture.monitor_destroyed = true;
    fixture.changed.notify_all();
}
Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::make_unique<MonitorImpl>()));
}
void NativeMonitor::wake() noexcept {
    std::lock_guard lock(fixture.mutex);
    fixture.wake = true;
    ++fixture.wake_calls;
    fixture.changed.notify_all();
}
Result<ProcessExitEvents> NativeMonitor::wait(bool polling) noexcept {
    std::unique_lock lock(fixture.mutex);
    fixture.waiting = true;
    fixture.polling = polling;
    fixture.changed.notify_all();
    const auto ready = [] {
        return fixture.wake || !fixture.events.empty() || fixture.cleanup;
    };
    if (polling) {
        if (!fixture.changed.wait_for(lock, fallback_interval, ready)) {
            ++fixture.timer_returns;
        }
    } else {
        fixture.changed.wait(lock, ready);
    }
    fixture.waiting = false;
    fixture.wake = false;
    ProcessExitEvents events;
    while (!fixture.events.empty() && events.count < events.capacity) {
        const auto id = fixture.events.front();
        fixture.events.pop_front();
        events.ids[events.count++] = id;
        ++fixture.delivered;
        for (const auto& child : fixture.children) {
            if (child->id == id) {
                child->notified = true;
            }
        }
    }
    if (events.count != 0) {
        ++fixture.batches;
        if (fixture.hold_batch) {
            fixture.batch_taken = true;
            fixture.changed.notify_all();
            fixture.changed.wait(lock, [] { return fixture.release_batch || fixture.cleanup; });
        }
        if (fixture.monitor_failure) {
            fixture.monitor_failure = false;
            return std::unexpected(Error{ErrorCode::SystemError, ErrorOperation::Observe});
        }
    }
    return events;
}

NativeProcess::NativeProcess(std::unique_ptr<ProcessImpl> impl) : impl_(std::move(impl)) {
}
NativeProcess::~NativeProcess() {
    std::lock_guard lock(fixture.mutex);
    impl_->child->released = true;
    fixture.changed.notify_all();
}
Result<std::unique_ptr<NativeProcess>> NativeProcess::spawn(const SpawnOptions&, NativeMonitor&,
                                                          ProcessId id, const ChildPipe*) {
    auto child = std::make_shared<ChildState>();
    child->id = id;
    {
        std::unique_lock lock(fixture.mutex);
        fixture.children.push_back(child);
        if (fixture.hold_spawn) {
            fixture.spawn_entered = true;
            fixture.events.push_back(id);
            fixture.changed.notify_all();
            fixture.changed.wait(lock, [] { return fixture.publish || fixture.cleanup; });
            if (fixture.fail_spawn) {
                return std::unexpected(Error{ErrorCode::SystemError, ErrorOperation::Spawn});
            }
        }
    }
    auto impl = std::make_unique<ProcessImpl>();
    impl->child = std::move(child);
    return std::unique_ptr<NativeProcess>(new NativeProcess(std::move(impl)));
}
std::uint64_t NativeProcess::pid() const noexcept {
    return reused_pid;
}
bool NativeProcess::event_driven() const noexcept {
    // Never fake fallback eligibility: the real observer must retain the event.
    return true;
}
Result<std::optional<ExitStatus>> NativeProcess::poll() noexcept {
    std::lock_guard lock(fixture.mutex);
    auto& child = *impl_->child;
    ++child.polls;
    if (fixture.cleanup) {
        return std::optional(ExitStatus{ExitKind::Code, exit_code});
    }
    if (child.error) {
        return std::unexpected(*child.error);
    }
    if (!child.notified || !child.ready) {
        return std::optional<ExitStatus>{};
    }
    if (child.empty_remaining > 0) {
        --child.empty_remaining;
        ++child.empty_results;
        fixture.changed.notify_all();
        return std::optional<ExitStatus>{};
    }
    ++child.exits;
    return std::optional(ExitStatus{ExitKind::Code, exit_code});
}
Result<TerminateResult> NativeProcess::terminate() noexcept {
    std::lock_guard lock(fixture.mutex);
    ++impl_->child->signals;
    return TerminateResult::Requested;
}
}

namespace {
void progress(ProcessService& service, int empty_polls) {
    auto child = spawn(service);
    auto native = state(child.id());
    {
        std::lock_guard lock(fixture.mutex);
        native->empty_remaining = empty_polls;
    }
    require_stays_idle();
    int wakes;
    int timers;
    {
        std::lock_guard lock(fixture.mutex);
        wakes = fixture.wake_calls;
        timers = fixture.timer_returns;
    }
    notify_exit(child.id());
    await_reaped(native);
    require_stays_idle();
    {
        std::lock_guard lock(fixture.mutex);
        require(native->empty_results == empty_polls && native->exits == 1, "incorrect poll sequence");
        require(fixture.timer_returns - timers == empty_polls, "missing or excessive timed retries");
        require(fixture.wake_calls == wakes && fixture.delivered == 1, "extra wake rescued progress");
    }
    require_exit(child);
}

void early_publication(ProcessService& service, bool fail) {
    {
        std::lock_guard lock(fixture.mutex);
        fixture.hold_spawn = true;
        fixture.fail_spawn = fail;
    }
    std::optional<vinc::Result<Process>> result;
    std::jthread launcher([&] { result.emplace(service.spawn(options())); });
    Cleanup cleanup;
    await_fixture([] {
        return fixture.spawn_entered && fixture.delivered == 1 && fixture.waiting && !fixture.polling;
    }, "early notification was not processed before publication");
    ProcessId old_id;
    {
        std::lock_guard lock(fixture.mutex);
        old_id = fixture.children.front()->id;
        fixture.publish = true;
        fixture.changed.notify_all();
    }
    launcher.join();
    require(result.has_value(), "missing spawn outcome");
    if (!fail) {
        require(bool(*result), "early spawn failed");
        auto child = std::move(**result);
        await_reaped(state(child.id()));
        require_exit(child);
        return;
    }
    require(!*result, "failed spawn unexpectedly succeeded");
    {
        std::lock_guard lock(fixture.mutex);
        fixture.hold_spawn = false;
    }
    auto replacement = spawn(service);
    require(replacement.id() != old_id, "service identity was reused");
    notify_exit(old_id);
    await_fixture([] { return fixture.delivered == 2 && fixture.waiting; }, "stale failed-spawn event not consumed");
    require_stays_idle();
    notify_exit(replacement.id());
    await_reaped(state(replacement.id()));
}

void stale_event(ProcessService& service, bool forget) {
    auto old = spawn(service);
    auto old_native = state(old.id());
    await_idle();
    {
        std::lock_guard lock(fixture.mutex);
        old_native->empty_remaining = 0;
        fixture.hold_batch = true;
    }
    notify_exit(old.id());
    await_fixture([] { return fixture.batch_taken; }, "event not held in local batch");
    // The observer has already dequeued the event, but has not applied it yet.
    require_exit(old);
    if (forget) {
        require(bool(service.forget(old.id())), "forget completed record");
    }
    auto replacement = spawn(service);
    require(old.pid() == replacement.pid() && old.id() != replacement.id(), "PID reuse fixture invalid");
    {
        std::lock_guard lock(fixture.mutex);
        fixture.release_batch = true;
        fixture.changed.notify_all();
    }
    await_fixture([] { return fixture.waiting && !fixture.wake; }, "held batch not applied");
    require_stays_idle();
    {
        std::lock_guard lock(fixture.mutex);
        fixture.hold_batch = false;
    }
    notify_exit(old.id());
    await_fixture([] { return fixture.delivered == 2 && fixture.waiting; }, "duplicate old event not applied");
    require_stays_idle();
    notify_exit(replacement.id());
    await_reaped(state(replacement.id()));
    require_exit(replacement);
}

void batches(ProcessService& service) {
    constexpr std::size_t child_count = detail::ProcessExitEvents::capacity + 8;
    std::vector<Process> children;
    for (std::size_t i = 0; i < child_count; ++i) {
        children.push_back(spawn(service));
    }
    await_idle();
    {
        std::lock_guard lock(fixture.mutex);
        for (const auto& child : children) {
            fixture.events.push_back(child.id());
        }
        fixture.events.push_back(children.front().id());
        fixture.events.push_back(ProcessId{std::numeric_limits<std::uint64_t>::max()});
        fixture.wake = true; // A user wake can coexist with native exit events.
        fixture.changed.notify_all();
    }
    for (auto& child : children) {
        await_reaped(state(child.id()));
    }
    require_stays_idle();
    {
        std::lock_guard lock(fixture.mutex);
        require(fixture.delivered == child_count + 2 && fixture.batches == 2, "batch boundary lost events");
        for (const auto& child : fixture.children) {
            require(child->exits == 1 && child->empty_results == 1, "duplicate or missed observation");
        }
    }
}

void waiter_lifetime(ProcessService& service) {
    auto child = spawn(service);
    auto native = state(child.id());
    {
        std::lock_guard lock(fixture.mutex);
        native->empty_remaining = held_empty_polls;
    }
    std::stop_source cancellation;
    std::optional<vinc::Result<ExitStatus>> cancelled;
    std::optional<vinc::Result<ExitStatus>> timed_out;
    std::optional<vinc::Result<ExitStatus>> completed;
    bool timeout_preserved_owner = false;
    int wakes;
    {
        std::lock_guard lock(fixture.mutex);
        wakes = fixture.wake_calls;
    }
    std::jthread first([&] { cancelled.emplace(child.wait_until(Clock::now() + fixture_timeout, cancellation.get_token())); });
    std::jthread second([&] {
        timed_out.emplace(child.wait_until(Clock::now() + waiter_deadline));
        std::lock_guard lock(fixture.mutex);
        timeout_preserved_owner = native->empty_results > 0 && !native->released;
    });
    std::jthread third([&] { completed.emplace(child.wait_until(Clock::now() + fixture_timeout)); });
    Cleanup cleanup;
    await_fixture([&] { return fixture.wake_calls >= wakes + 3 && fixture.waiting && !fixture.wake; },
                  "public waiters did not enter before notification");
    notify_exit(child.id());
    await_fixture([&] { return native->empty_results > 0 && fixture.polling; }, "pending fallback not entered");
    cancellation.request_stop();
    first.join();
    second.join();
    require(cancelled && !*cancelled && cancelled->error().is(ErrorCode::Cancelled), "wait cancellation lost");
    require(timed_out && !*timed_out && timed_out->error().is(ErrorCode::Timeout), "wait deadline lost");
    require(timeout_preserved_owner, "deadline did not expire during pending observation");
    {
        std::lock_guard lock(fixture.mutex);
        require(!native->released, "waiter cancellation abandoned owner");
        native->empty_remaining = 0; // No event or public wake accompanies readiness.
    }
    await_reaped(native);
    third.join();
    require(completed && bool(*completed) && (*completed)->value == exit_code, "other waiter lost result");
    require_stays_idle();
}

void faults(ProcessService& service, std::string_view mode) {
    auto child = spawn(service);
    auto native = state(child.id());
    await_idle();
    {
        std::lock_guard lock(fixture.mutex);
        native->empty_remaining = 2;
        if (mode == "monitor-error") {
            fixture.monitor_failure = true;
        } else {
            const auto code = mode == "ownership-lost" ? ErrorCode::ObservationLost : ErrorCode::SystemError;
            native->error = vinc::Error{code, ErrorOperation::Observe};
        }
    }
    notify_exit(child.id());
    if (mode == "monitor-error") {
        await_reaped(native);
        const auto error = service.monitor_error();
        require(error && error->is(ErrorCode::SystemError), "mixed batch lost monitor diagnostic");
        require_exit(child);
        return;
    }
    if (mode == "ownership-lost") {
        await_reaped(native);
        auto terminated = child.terminate();
        require(!terminated && terminated.error().is(ErrorCode::ObservationLost), "lost owner remains controllable");
        {
            std::lock_guard lock(fixture.mutex);
            require(native->signals == 0 && native->exits == 0, "fabricated exit or signalled reused PID");
        }
        notify_exit(child.id());
        await_fixture([] { return fixture.delivered == 2 && fixture.waiting; }, "late lost-owner event not consumed");
        require_stays_idle();
        require(bool(service.forget(child.id())), "cannot release lost-owner record");
        return;
    }
    await_fixture([&] { return native->polls >= 2 && fixture.polling; }, "ordinary observation error not retried");
    auto diagnostic = child.observation_error();
    require(diagnostic && diagnostic->is(ErrorCode::SystemError), "first diagnostic not retained");
    {
        std::lock_guard lock(fixture.mutex);
        native->error.reset();
    }
    await_reaped(native);
    require_exit(child);
    require(child.observation_error() == diagnostic, "recovery erased diagnostic");
    require_stays_idle();
    require(bool(service.forget(child.id())), "cannot release recovered record");
}

void shutdown_lifetime(ProcessService& service) {
    auto child = spawn(service);
    const auto id = child.id();
    auto native = state(id);
    {
        std::lock_guard lock(fixture.mutex);
        native->empty_remaining = held_empty_polls;
    }
    notify_exit(id);
    await_fixture([&] { return native->empty_results > 0 && fixture.polling; }, "missing pending observation");
    child = {};
    auto timed = service.shutdown_until(Clock::now());
    require(!timed && timed.error().is(ErrorCode::Timeout), "shutdown abandoned pending owner");
    {
        std::lock_guard lock(fixture.mutex);
        native->empty_remaining = 0;
    }
    await_reaped(native);
    require(bool(service.shutdown_until(Clock::now() + fixture_timeout)), "shutdown did not finish");
    auto retained = service.find(id);
    require(bool(retained), "dropping public view lost record");
    require_exit(*retained);
    require(bool(service.forget(id)), "shutdown record not reclaimable");
}

void run(std::string_view mode) {
    auto created = ProcessService::create(ServiceOptions{mode == "failed-publication" ? 1u : 64u});
    require(bool(created), "create service");
    auto service = std::move(*created);
    Cleanup cleanup;
    if (mode == "one-shot" || mode == "repeated-empty" || mode == "immediate") {
        int empty_polls = 1;
        if (mode == "immediate") {
            empty_polls = 0;
        } else if (mode == "repeated-empty") {
            empty_polls = 3;
        }
        progress(service, empty_polls);
    } else if (mode == "early-publication" || mode == "failed-publication") {
        early_publication(service, mode == "failed-publication");
    } else if (mode == "stale-retained" || mode == "stale-forgotten") {
        stale_event(service, mode == "stale-forgotten");
    } else if (mode == "batches") {
        batches(service);
    } else if (mode == "waiters") {
        waiter_lifetime(service);
    } else if (mode == "monitor-error" || mode == "poll-error" || mode == "ownership-lost") {
        faults(service, mode);
    } else if (mode == "shutdown") {
        shutdown_lifetime(service);
    } else {
        throw std::runtime_error("unknown test case");
    }
    require(bool(service.shutdown_until(Clock::now() + fixture_timeout)), "final shutdown failed");
}

void record_state() {
    // Inspect state cleanup directly under the same lock used by every public
    // observation path; an ownerless record would otherwise hide a stale bit.
    auto monitor = detail::NativeMonitor::create();
    require(bool(monitor), "create monitor");
    for (const bool lose_owner : {false, true}) {
        detail::ProcessRecord record;
        auto id = detail::next_id();
        require(bool(id), "allocate identity");
        record.id = *id;
        auto native = detail::NativeProcess::spawn(options(), **monitor, record.id);
        require(bool(native), "create record owner");
        record.native = std::move(*native);
        record.starting = false;
        record.exit_notification_pending = true;
        auto child = state(record.id);
        {
            std::lock_guard lock(fixture.mutex);
            child->notified = true;
            child->empty_remaining = 1;
        }
        std::lock_guard record_lock(record.mutex);
        detail::observe(record);
        require(record.native && record.exit_notification_pending && !record.error,
                "empty poll discarded responsibility or invented error");
        {
            std::lock_guard lock(fixture.mutex);
            child->error = vinc::Error{ErrorCode::SystemError, ErrorOperation::Observe};
        }
        detail::observe(record);
        require(record.native && record.exit_notification_pending && record.error,
                "ordinary error discarded ownership or pending state");
        {
            std::lock_guard lock(fixture.mutex);
            if (lose_owner) {
                child->error = vinc::Error{ErrorCode::ObservationLost, ErrorOperation::Observe};
            } else {
                child->error.reset();
            }
        }
        detail::observe(record);
        require(!record.native && !record.exit_notification_pending,
                "completed/lost observation retained pending state");
        require(record.exit.has_value() != lose_owner, "lost owner fabricated success");
        require(record.error && record.error->is(ErrorCode::SystemError), "first diagnostic overwritten");
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "one test case is required");
        if (std::string_view(argv[1]) == "record-state") {
            record_state();
        } else {
            run(argv[1]);
        }
        await_fixture([] { return fixture.monitor_destroyed; }, "monitor outlived cleanup");
        std::cout << argv[1] << ": passed; monitor released\n";
    } catch (const std::exception& error) {
        std::cerr << "exit-progress contract failure: " << error.what() << std::endl;
        // A failed barrier must not leave detached fixture threads using globals.
        std::_Exit(1);
    }
}
