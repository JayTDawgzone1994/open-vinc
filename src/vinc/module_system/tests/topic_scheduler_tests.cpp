#include <cstdint>
#include <limits>
#include <system_error>
namespace {
std::uint32_t fail_start_at = (std::numeric_limits<std::uint32_t>::max)();
void topic_scheduler_test_before_start(std::uint32_t index) {
    if (index == fail_start_at) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
}
}
// Compile the production scheduler with a test-only thread-creation checkpoint.
#include "../src/runtime/topic_scheduler.cpp"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto gate_timeout = 3s;
constexpr std::size_t replacement_count = 1000;
constexpr std::size_t hot_turns = 20;

void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
template<class T>
void take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}
template<class F>
auto task(F work) {
    return std::make_shared<detail::TopicScheduler::Task>(std::move(work));
}
void wait(std::future<void>& value) {
    check(value.wait_for(gate_timeout) == std::future_status::ready, "controlled checkpoint timed out");
    value.get();
}

void parallel_and_same_topic() {
    std::promise<void> first_entered;
    std::promise<void> second_entered;
    auto first = first_entered.get_future();
    auto second = second_entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    std::promise<void> replacement_ran;
    auto replacement = replacement_ran.get_future();
    std::atomic<int> active{0};
    std::atomic<int> replacement_runs{0};
    detail::TopicScheduler scheduler{[] {}};
    take(scheduler.start(2));
    take(scheduler.submit("a", task([&]() -> Result<bool> {
        check(active.fetch_add(1) == 0, "same Topic overlapped");
        first_entered.set_value();
        check(released.wait_for(gate_timeout) == std::future_status::ready, "release first Topic");
        active.fetch_sub(1);
        return false;
    })));
    wait(first);
    for (std::size_t index = 0; index < replacement_count; ++index) {
        take(scheduler.submit("a", task([&]() -> Result<bool> {
            check(active.fetch_add(1) == 0, "replacement bypassed active Topic");
            ++replacement_runs;
            active.fetch_sub(1);
            replacement_ran.set_value();
            return false;
        })));
    }
    take(scheduler.submit("b", task([&]() -> Result<bool> {
        check(active.load() == 1, "different Topics did not overlap");
        check(replacement_runs.load() == 0, "same Topic ran before release");
        second_entered.set_value();
        return false;
    })));
    wait(second);
    release.set_value();
    wait(replacement);
    scheduler.stop();
    check(replacement_runs == 1, "pending metadata must coalesce, not accumulate");
    check(!scheduler.failure(), "parallel scheduler failure");
}

void fairness() {
    std::promise<void> entered;
    auto first = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    std::promise<void> cold_done;
    auto cold = cold_done.get_future();
    std::promise<void> hot_done;
    auto hot = hot_done.get_future();
    std::atomic<std::size_t> turns{0};
    detail::TopicScheduler scheduler{[] {}};
    take(scheduler.start(1));
    take(scheduler.submit("hot", task([&]() -> Result<bool> {
        const auto turn = ++turns;
        if (turn == 1) {
            entered.set_value();
            check(released.wait_for(gate_timeout) == std::future_status::ready, "release hot Topic");
        }
        if (turn == hot_turns) {
            hot_done.set_value();
            return false;
        }
        return true;
    })));
    wait(first);
    take(scheduler.submit("cold", task([&]() -> Result<bool> {
        check(turns == 1, "hot Topic monopolized worker");
        cold_done.set_value();
        return false;
    })));
    release.set_value();
    wait(cold);
    wait(hot);
    scheduler.stop();
    check(!scheduler.failure(), "fairness scheduler failure");
}

void rollback_and_capacity() {
    detail::TopicScheduler invalid{[] {}};
    auto rejected = invalid.start(config::module_topic_worker_limit + 1);
    check(!rejected && rejected.error().is(module_system::ErrorCode::InvalidArgument), "worker limit");
    fail_start_at = 1;
    detail::TopicScheduler partial{[] {}};
    auto failed = partial.start(2);
    fail_start_at = (std::numeric_limits<std::uint32_t>::max)();
    check(!failed && failed.error().is(module_system::ErrorCode::ThreadFailed), "partial creation failure");
    check(partial.thread_ids().empty(), "failed startup left a joinable worker");

    std::promise<void> entered;
    auto first = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    detail::TopicScheduler bounded{[] {}};
    take(bounded.start(1));
    take(bounded.submit("held", task([&]() -> Result<bool> {
        entered.set_value();
        check(released.wait_for(gate_timeout) == std::future_status::ready, "release bounded queue");
        return false;
    })));
    wait(first);
    for (std::size_t index = 1; index < config::module_callback_limit; ++index) {
        take(bounded.submit(std::to_string(index), task([]() -> Result<bool> { return false; })));
    }
    auto full = bounded.submit("excess", task([]() -> Result<bool> { return false; }));
    check(!full && full.error().is(module_system::ErrorCode::CapacityExceeded), "ready queue limit");
    release.set_value();
    bounded.stop();
}

void failure_stops_dispatch() {
    std::promise<void> wake;
    auto woken = wake.get_future();
    detail::TopicScheduler scheduler{[&] { wake.set_value(); }};
    take(scheduler.start(1));
    take(scheduler.submit("bad", task([]() -> Result<bool> {
        return std::unexpected(Error{module_system::ErrorCode::BusFailed, module_system::ErrorOperation::Pump});
    })));
    wait(woken);
    auto failure = scheduler.failure();
    check(failure && failure->is(module_system::ErrorCode::BusFailed), "preserve first failure");
    auto rejected = scheduler.submit("later", task([]() -> Result<bool> { return false; }));
    check(!rejected && rejected.error().is(module_system::ErrorCode::BusFailed), "stop admission after fault");
    scheduler.stop();
}
}

int main() {
    try {
        parallel_and_same_topic();
        fairness();
        rollback_and_capacity();
        failure_stops_dispatch();
        std::cout << "Topic scheduler tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
