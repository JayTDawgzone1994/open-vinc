#include "plugin_test_support.hpp"
#include "control_wakeup_hooks.hpp"
#include "../src/host/module_manager.cpp"

namespace vinc::rpc::detail {
// Existing test friendship controls the real queue, without a production gate
// or fabricated QueueFull result. No worker uses the dummy occupied slots.
struct ManagerTests {
    static void saturate(RpcManager& manager) {
        manager.worker_.request_stop();
        manager.changed_.notify_all();
        manager.worker_.join();
        std::lock_guard lock(manager.mutex_);
        test::plugin::check(manager.input_.empty(), "RPC queue initially empty");
        manager.input_.resize(config::rpc_control_limit);
    }
    static void refill(RpcManager& manager) {
        std::lock_guard lock(manager.mutex_);
        manager.input_.clear();
        manager.input_.resize(config::rpc_control_limit);
    }
    static void release(RpcManager& manager) {
        std::lock_guard lock(manager.mutex_);
        manager.input_.clear();
    }
    static std::vector<std::uint64_t> accepted(RpcManager& manager) {
        std::lock_guard lock(manager.mutex_);
        std::vector<std::uint64_t> result;
        for (const auto& input : manager.input_) {
            result.push_back(input.frame.request);
        }
        return result;
    }
};
}

namespace {
using namespace vinc;
using namespace vinc::test::plugin;
namespace probe = test::control_wakeup;
constexpr std::uint64_t burst_request = 2000000;
constexpr std::uint64_t rpc_request = 3000000;
constexpr std::uint64_t other_request = 4000000;
constexpr std::size_t burst_frames = 40;
constexpr int required_full_retries = 5;
struct Taken {
    std::uint64_t request;
    std::size_t pass;
    std::size_t waits;
};
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<control::Session>> sessions;
    std::vector<std::uint64_t> queued;
    std::vector<Taken> taken;
    std::size_t pass = 0;
    std::size_t waits = 0;
    bool health_due = false;
    Deadline health_deadline;
    bool armed = false;
    bool entered = false;
    bool released = false;
    bool expired = false;
    std::optional<std::cv_status> gated_wait;
    rpc::detail::RpcManager* rpc = nullptr;
    std::vector<Clock::time_point> full_at;
    std::vector<std::size_t> full_waits;
    std::vector<bool> full_health;
    std::vector<std::uint64_t> accepted;
} state;

struct Unblock {
    ~Unblock() {
        std::lock_guard lock(state.mutex);
        state.released = true;
        state.armed = false;
        state.changed.notify_all();
    }
};
template<class Predicate>
void await(std::unique_lock<std::mutex>& lock, Predicate predicate, const char* reason) {
    check(state.changed.wait_for(lock, wait_budget, predicate), reason);
    check(!state.expired, "scheduler gate expired");
}
void arm() {
    std::unique_lock lock(state.mutex);
    state.armed = true;
    state.entered = false;
    state.released = false;
    state.gated_wait.reset();
    await(lock, [] { return state.entered; }, "supervisor pre-wait gate");
}
void release() {
    std::lock_guard lock(state.mutex);
    state.released = true;
    state.armed = false;
    state.changed.notify_all();
}
void send(control::Session& session, std::uint64_t request) {
    check(bool(session.send({control::Kind::Request, detail::commands, detail::execute, request, {}})),
          "scripted command sent");
}
bool has_taken(std::uint64_t request) {
    return std::ranges::any_of(state.taken, [=](const Taken& item) { return item.request == request; });
}

void fairness() {
    arm();
    send(*state.sessions[0], burst_request);
    send(*state.sessions[1], other_request);
    {
        std::unique_lock lock(state.mutex);
        await(lock, [] { return state.queued.size() >= burst_frames + 1; }, "two inboxes queued before scan");
    }
    for (const auto& session : state.sessions) {
        check(session->state() == control::State::Open, "queued publication completed before gate release");
    }
    release();
    std::unique_lock lock(state.mutex);
    await(lock, [] { return has_taken(burst_request + burst_frames - 1) && has_taken(other_request); },
          "all bounded batches consumed without new traffic");
    const auto first = std::ranges::find(state.taken, burst_request, &Taken::request);
    const auto other = std::ranges::find(state.taken, other_request, &Taken::request);
    check(first != state.taken.end() && other != state.taken.end(), "both sources observed");
    check(other->pass == first->pass, "second Module progresses in first fair pass");
    for (std::size_t index = 0; index < burst_frames; ++index) {
        const auto item = std::ranges::find(state.taken, burst_request + index, &Taken::request);
        check(item != state.taken.end(), "no forgotten backlog");
        check(item->pass == first->pass + index / control_messages_per_pass, "16 frames per fair pass");
        check(item->waits == first->waits, "budget exhaustion does not sleep with runnable backlog");
    }
    std::vector<std::uint64_t> order;
    for (const auto& item : state.taken) {
        if (item.request >= burst_request && item.request < burst_request + burst_frames) {
            order.push_back(item.request);
        }
    }
    check(std::ranges::is_sorted(order), "Session FIFO preserved across passes");
}

void saturation(ModuleManager& manager, MID other_mid) {
    send(*state.sessions[0], rpc_request);
    {
        std::unique_lock lock(state.mutex);
        await(lock, [] { return !state.full_at.empty(); }, "real RPC queue full reached");
    }
    // Keep another Session active during backpressure. Its progress cannot make
    // deferred input immediately runnable or postpone absolute health ticks.
    for (std::uint64_t index = 1; index <= 32; ++index) {
        send(*state.sessions[1], other_request + index);
        std::unique_lock lock(state.mutex);
        await(lock, [=] { return has_taken(other_request + index); }, "other Module progresses during RPC full");
    }
    check(bool(manager.stop_module(other_mid, Clock::now() + wait_budget)),
          "other Module stops while RPC remains full");
    // No new traffic now. Repeated health-only retries prove fallback continues;
    // wait-count and time-spacing assertions detect self-wake/spinning retries.
    {
        std::unique_lock lock(state.mutex);
        const auto quiet_pass = state.pass;
        const auto target_retries = state.full_at.size() + required_full_retries;
        await(lock, [=] { return state.full_at.size() >= target_retries; }, "fallback retries without traffic");
        // At most one health pass and one coalesced stale hint per retry,
        // plus the final closed-peer hint. A runnable/deferred mixup spins here.
        constexpr std::size_t quiet_pass_limit = 2 * required_full_retries + 4;
        check(state.pass - quiet_pass <= quiet_pass_limit, "RPC saturation does not busy-loop supervisor");
        for (std::size_t index = 1; index < state.full_at.size(); ++index) {
            check(state.full_health[index], "deferred retry only on health pass");
            if (index >= 2) {
                check(state.full_at[index] - state.full_at[index - 1] >= supervisor_poll_interval,
                      "full retries respect fallback cadence");
            }
            check(state.full_waits[index] > state.full_waits[index - 1], "supervisor waits between full retries");
        }
        check(!has_taken(rpc_request + 3), "trailing frame cannot pass deferred RPC");
        rpc::detail::ManagerTests::release(*state.rpc);
        await(lock, [] { return state.accepted.size() == 3 && has_taken(rpc_request + 3); },
              "capacity release admits original requests and trailing marker");
        const std::vector<std::uint64_t> expected{rpc_request, rpc_request + 1, rpc_request + 2};
        check(state.accepted == expected && rpc::detail::ManagerTests::accepted(*state.rpc) == expected,
              "real RPC admission occurs once per original request, FIFO");
    }
}
}

namespace vinc::test::control_wakeup {
void attached(const std::shared_ptr<control::Session>& session) {
    std::lock_guard lock(state.mutex);
    state.sessions.push_back(session);
}
void pass_started(bool health_due, Deadline health_deadline) {
    std::lock_guard lock(state.mutex);
    ++state.pass;
    state.health_due = health_due;
    state.health_deadline = health_deadline;
}
void frame_event(Point point, control::Role role, std::uint64_t, const control::Frame& frame) {
    if (role != control::Role::Central || frame.request < burst_request) {
        return;
    }
    std::lock_guard lock(state.mutex);
    if (point == Point::Queued) {
        state.queued.push_back(frame.request);
    } else if (point == Point::Taken) {
        state.taken.push_back({frame.request, state.pass, state.waits});
    }
    state.changed.notify_all();
}
void before_wait() {
    std::unique_lock lock(state.mutex);
    if (state.armed && !state.entered) {
        state.entered = true;
        state.changed.notify_all();
        if (!state.changed.wait_for(lock, wait_budget, [] { return state.released; })) {
            state.expired = true;
        }
    }
}
void after_wait(std::cv_status status) {
    std::lock_guard lock(state.mutex);
    ++state.waits;
    if (state.entered && state.released && !state.gated_wait) {
        state.gated_wait = status;
    }
    state.changed.notify_all();
}
void before_rpc_submit(rpc::detail::RpcManager& manager, std::uint64_t, const control::Frame&) {
    std::lock_guard lock(state.mutex);
    if (!state.rpc) {
        rpc::detail::ManagerTests::saturate(manager);
        state.rpc = &manager;
    }
}
void after_rpc_submit(std::uint64_t, const control::Frame& frame, const Result<void>& result) {
    std::lock_guard lock(state.mutex);
    if (result) {
        state.accepted.push_back(frame.request);
    } else if (result.error().is(rpc::ErrorCode::QueueFull)) {
        check(frame.request == rpc_request, "same deferred request retained until accepted");
        state.full_at.push_back(state.health_deadline);
        state.full_waits.push_back(state.waits);
        state.full_health.push_back(state.health_due);
    }
    state.changed.notify_all();
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "scripted Host argument required");
        Scope scope;
        Unblock unblock_before_cleanup;
        const auto host = take(osal::path_from_utf8(argv[1]));
        scope.create(host);
        ModuleType type;
        type.name = "scripted";
        type.library = host;
        check(bool(scope.manager->register_module_type(std::move(type))), "register scripted type");
        const auto first = take(scope.manager->start_module("scripted", {"first"}));
        check(bool(scope.manager->wait_running(first, Clock::now() + wait_budget)), "first Module ready");
        const auto second = take(scope.manager->start_module("scripted", {"second"}));
        check(bool(scope.manager->wait_running(second, Clock::now() + wait_budget)), "second Module ready");
        fairness();
        saturation(*scope.manager, second);
        {
            std::unique_lock lock(state.mutex);
            rpc::detail::ManagerTests::refill(*state.rpc);
            state.accepted.clear();
            const auto previous_full = state.full_at.size();
            lock.unlock();
            send(*state.sessions[0], rpc_request);
            lock.lock();
            await(lock, [=] { return state.full_at.size() > previous_full; }, "blocked Module before shutdown");
        }
        arm();
        scope.manager->begin_shutdown();
        release();
        check(bool(scope.manager->wait_shutdown(Clock::now() + wait_budget)), "confirmed shutdown with parked RPC worker");
        {
            std::lock_guard lock(state.mutex);
            check(state.gated_wait == std::cv_status::no_timeout, "management signal survives pre-wait handoff");
            check(state.accepted.empty(), "blocked Module exits without downstream capacity becoming available");
        }
        for (const auto& snapshot : scope.manager->snapshots()) {
            check(snapshot.last_exit && snapshot.last_exit->kind == osal::ExitKind::Code &&
                  snapshot.last_exit->value == 0 && !snapshot.last_failure, "both scripted peers stopped normally");
        }
        state.sessions.clear();
        check(scope.finish(), "all owned processes reclaimed");
        std::cout << "PASS: fair FIFO batches, RPC full waits/retries/admission, other Module progress, shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
