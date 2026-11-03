#include "coordinator_fixture.hpp"
#include <vinc/osal/dynamic_library.hpp>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace vinc::osal;
using namespace vinc::osal::detail;
using namespace std::chrono_literals;
constexpr auto wait_budget = 2s;
constexpr auto blocked_timeout = 30ms;
constexpr int contention_iterations = 500;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}
const CoordinatorFixture* query(DynamicLibrary& library) {
    auto function = take(library.function("coordinator_fixture"));
    // Native ABI boundary: the fixture export has exactly CoordinatorQuery's
    // signature and returns a readable same-build table alive until unload.
    return reinterpret_cast<CoordinatorQuery>(function)();
}

void standalone() {
    {
        auto first = take(ResourceGuard::try_acquire());
        auto second = ResourceGuard::try_acquire();
        check(!second && second.error().is(ErrorCode::WouldBlock), "non-recursive try must not block");
        auto owner = take(resource_coordination());
        auto late = bind_resource_coordination(owner);
        check(!late && late.error().is(ErrorCode::CoordinatorInUse), "used default cannot be replaced");
        auto moved = std::move(first);
    }
    check(bool(ResourceGuard::try_acquire()), "moved guard releases exactly once");
}

void libraries(const char* first_path, const char* second_path, bool late_binding) {
    auto owner = take(resource_coordination());
    auto first_library = take(DynamicLibrary::open(std::filesystem::absolute(first_path)));
    auto second_library = take(DynamicLibrary::open(std::filesystem::absolute(second_path)));
    const auto* first = query(first_library);
    const auto* second = query(second_library);

    if (late_binding) {
        check(bool(first->enter(true, Deadline::max(), {})), "pre-bind standalone operation");
        first->leave();
        auto rejected = first->bind(owner);
        check(!rejected && rejected.error().is(ErrorCode::CoordinatorInUse), "late library binding rejected");
        check(first->identity() != owner.context, "independent static OSAL image proven");
        return;
    }

    auto invalid = owner;
    invalid.version += 1;
    auto version = first->bind(invalid);
    check(!version && version.error().is(ErrorCode::IncompatibleCoordinator), "version rejected");
    invalid = owner;
    invalid.size = 0;
    check(!first->bind(invalid), "undersized table rejected");
    invalid = owner;
    invalid.release = nullptr;
    check(!first->bind(invalid), "missing release rejected");
    invalid = owner;
    invalid.acquire = nullptr;
    check(!first->bind(invalid), "missing acquire rejected");
    invalid = owner;
    invalid.context = nullptr;
    check(!first->bind(invalid), "missing owner rejected");
    check(bool(first->bind(owner)), "invalid tables did not consume binding");
    check(bool(second->bind(owner)), "second image binds");
    auto duplicate = first->bind(owner);
    check(!duplicate && duplicate.error().is(ErrorCode::CoordinatorAlreadyBound), "duplicate rejected");
    check(first->identity() == owner.context && second->identity() == owner.context, "same Host owner");

    check(bool(first->enter(true, Deadline::max(), {})), "first image holds coordinator");
    auto busy = second->enter(true, Deadline::max(), {});
    check(!busy && busy.error().is(ErrorCode::WouldBlock), "second image cannot overlap");
    auto host_busy = ResourceGuard::try_acquire();
    check(!host_busy && host_busy.error().is(ErrorCode::WouldBlock), "Host shares same exclusion");
    auto timeout = second->enter(false, std::chrono::steady_clock::now() + blocked_timeout, {});
    check(!timeout && timeout.error().is(ErrorCode::Timeout), "deadline honored under contention");

    std::stop_source cancellation;
    std::atomic<bool> waiting{false};
    Result<void> cancelled;
    std::jthread waiter([&] {
        waiting.store(true);
        cancelled = second->enter(false, std::chrono::steady_clock::now() + wait_budget, cancellation.get_token());
    });
    while (!waiting.load()) {
        std::this_thread::yield();
    }
    cancellation.request_stop();
    waiter.join();
    check(!cancelled && cancelled.error().is(ErrorCode::Cancelled), "cancellation wakes a waiting image");
    first->leave();

    check(bool(first->enter(true, Deadline::max(), {})), "hold for indefinite waiter");
    Result<void> awakened;
    std::jthread indefinite([&] {
        awakened = second->enter(false, Deadline::max(), {});
        if (awakened) {
            second->leave();
        }
    });
    first->leave();
    indefinite.join();
    check(bool(awakened), "indefinite wait wakes after release");

    // Real cross-image contention protects ordinary non-atomic application data.
    int shared_count = 0;
    std::atomic<bool> success{true};
    auto increment = [&](const CoordinatorFixture* image) {
        for (int iteration = 0; iteration < contention_iterations; ++iteration) {
            auto entered = image->enter(false, std::chrono::steady_clock::now() + wait_budget, {});
            if (!entered) {
                success.store(false);
                return;
            }
            ++shared_count;
            image->leave();
        }
    };
    std::jthread first_worker(increment, first);
    std::jthread second_worker(increment, second);
    for (int iteration = 0; iteration < contention_iterations; ++iteration) {
        auto guard = take(ResourceGuard::acquire_until(std::chrono::steady_clock::now() + wait_budget));
        ++shared_count;
    }
    first_worker.join();
    second_worker.join();
    check(success.load() && shared_count == contention_iterations * 3, "three images share exclusion");

    int first_unloaded = 0;
    int second_unloaded = 0;
    first->observe_unload(&first_unloaded);
    second->observe_unload(&second_unloaded);
    check(bool(first_library.close()), "unload first library");
    check(bool(second_library.close()), "unload second library");
    check(first_unloaded == 1 && second_unloaded == 1, "unload destructors retain live Host coordinator");
    check(bool(ResourceGuard::try_acquire()), "unload released all guards");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            standalone();
        } else {
            check(argc == 4, "expected two libraries and mode");
            libraries(argv[1], argv[2], std::string_view(argv[3]) == "late");
        }
        std::cout << "resource coordination passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
