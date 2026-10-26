#include "../../../../tests/support/manager.hpp"
#include <vinc/osal/path.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <future>
#include <iostream>
#include <thread>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
constexpr auto timeout = 10s;
constexpr auto observation_interval = 5ms;
constexpr auto contention_observation = 100ms;
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
template<class T> T take(vinc::Result<T> value) {
    if (!value) {
        throw std::runtime_error(vinc::error_to_string(value.error()));
    }
    return std::move(*value);
}
void ok(vinc::Result<void> value) {
    if (!value) {
        throw std::runtime_error(vinc::error_to_string(value.error()));
    }
}
vinc::Deadline deadline() {
    return std::chrono::steady_clock::now() + timeout;
}

void run(const std::filesystem::path& plugin) {
    auto manager = take(vinc::test::create_manager());
    const auto descriptor = take(vinc::detail::ManagerAccess::create_lock(*manager));
    auto observer = take(vinc::concurrency::InterprocessMutex::open(descriptor.name, vinc::central_owner));
    vinc::ModuleType type;
    type.name = "LockFixture";

    type.library = plugin;
    type.policy.max_restarts = 1;
    ok(manager->register_module_type(type));
    // The base name is fixed by this fixture: reuse it only after forgetting the old logical Module.
    auto first = take(manager->start_module(type.name));
    ok(manager->wait_running(first, deadline()));
    check(take(manager->command(first, {"hold", std::to_string(descriptor.id)}, deadline())) == 0,
          "initial publication");
    check(take(observer.owner()) == first, "Host must bind the granted MID");
    auto blocked = observer.try_lock();
    check(!blocked && blocked.error().is(vinc::concurrency::ErrorCode::WouldBlock), "live owner was reclaimed");
    (void)manager->command(first, {"crash"}, deadline());
    vinc::MID second = 0;
    const auto end = deadline();
    while (std::chrono::steady_clock::now() < end) {
        const auto snapshots = manager->snapshots();
        if (!snapshots.empty() && snapshots.front().mid != first &&
            snapshots.front().state == vinc::ModuleState::Running) {
            second = snapshots.front().mid;
            break;
        }
        std::this_thread::sleep_for(observation_interval);
    }
    check(second > first, "restart did not issue a new MID");
    check(take(manager->command(second, {"get", std::to_string(descriptor.id)}, deadline())) == 0,
          "unpublished candidate was exposed");
    take(manager->command(second, {"hold", std::to_string(descriptor.id)}, deadline()));
    take(manager->command(second, {"publish"}, deadline()));
    check(take(manager->command(second, {"get", std::to_string(descriptor.id)}, deadline())) == 1,
          "atomic publication not visible");
    auto central_guard = take(observer.try_lock());
    auto waiting = std::async(std::launch::async, [&] {
        return manager->command(second, {"hold", std::to_string(descriptor.id)}, deadline());
    });
    check(waiting.wait_for(contention_observation) == std::future_status::timeout, "contention should wait");
    ok(manager->stop_module(second, deadline()));
    check(waiting.wait_for(timeout) == std::future_status::ready, "stop did not cancel lock wait");
    (void)waiting.get();
    manager->begin_shutdown();
    ok(manager->wait_shutdown(deadline()));
    auto snapshot = manager->inspect(second);
    check(snapshot && snapshot->last_exit && snapshot->last_exit->value == 0,
          "cooperative cancellation should exit without OS kill");
    check(take(observer.owner()) == vinc::central_owner, "stop must not release another owner's lock");
    ok(central_guard.unlock());
    auto final = take(observer.try_lock());
    check(final.publication(), "stop changed publication");
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 2, "plugin path required");
        run(std::filesystem::absolute(argv[1]));
        std::cout << "PASS: Host lock identity, crash recovery, restart, publication, cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
