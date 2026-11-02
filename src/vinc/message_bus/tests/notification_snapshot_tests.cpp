#include "bus_test_hooks.hpp"
#include <vinc/message_bus/detail/notifications.hpp>
#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <condition_variable>
#include <future>
#include <stdexcept>

namespace vinc::message_bus::detail::testing {
thread_local std::function<void()> before_notification;

namespace {
using namespace std::chrono_literals;
constexpr auto test_timeout = 5s;
constexpr std::size_t replacement_count = 4000;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void ok(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

struct Receiver {
    std::string name = shared_name('n');
    osal::NotificationReceiver receiver = take(osal::NotificationReceiver::create(name));

    ~Receiver() {
        (void)osal::NotificationReceiver::unlink_name(name);
    }

    void consume() {
        ok(receiver.wait_until(std::chrono::steady_clock::now() + test_timeout));
    }

    void expect_empty() {
        auto result = receiver.wait_until(std::chrono::steady_clock::now());
        check(!result && result.error().is(osal::ErrorCode::Timeout), "unexpected generation notified");
    }
};

// Both sides use deadlines. Even a broken lock scope must release the paused
// sender before futures are destroyed; CTest supplies the outer deadlock bound.
class SendPause {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;

public:
    void pause() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        check(changed_.wait_for(lock, test_timeout, [&] { return released_; }), "send pause timed out");
    }

    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, test_timeout, [&] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
};

void test_inflight_retirement_and_generation() {
    Receiver old_receiver;
    Receiver new_receiver;
    auto totals = std::make_shared<NotificationTotals>();
    auto old_destination = std::make_shared<NotificationDestination>(NotificationTarget{2, old_receiver.name}, totals);
    auto new_destination = std::make_shared<NotificationDestination>(NotificationTarget{3, new_receiver.name}, totals);
    old_destination->reopen(std::chrono::steady_clock::now());
    new_destination->reopen(std::chrono::steady_clock::now());
    old_receiver.consume();
    new_receiver.consume();

    NotificationRoutes routes;
    routes.replace(std::make_shared<const NotificationDestinations>(NotificationDestinations{old_destination}));
    std::weak_ptr<NotificationDestination> retired = old_destination;
    old_destination.reset();

    SendPause pause;
    auto sending = std::async(std::launch::async, [&] {
        before_notification = [&] { pause.pause(); };
        routes.notify();
    });
    const bool entered = pause.wait_entered();
    if (!entered) {
        pause.release();
        sending.get();
        check(false, "notifier did not reach native-send boundary");
    }

    auto replacement = std::async(std::launch::async, [&] {
        routes.replace(std::make_shared<const NotificationDestinations>(NotificationDestinations{new_destination}));
    });
    const bool replaced_while_sending = replacement.wait_for(test_timeout) == std::future_status::ready;
    const bool old_still_alive = !retired.expired();
    pause.release();
    replacement.get();
    sending.get();

    check(replaced_while_sending, "native send must not hold the snapshot lock");
    check(old_still_alive, "in-flight snapshot must retain the retired sender");
    check(retired.expired(), "last snapshot release must reclaim retired destination");
    old_receiver.consume();
    new_receiver.expect_empty();
    routes.notify();
    new_receiver.consume();
    old_receiver.expect_empty();

    routes.replace({});
    routes.notify();
    new_receiver.expect_empty();
}

void test_reclamation_outside_lock() {
    NotificationRoutes routes;
    bool reclaimed = false;
    bool observed_replacement = false;
    auto next = std::make_shared<const NotificationDestinations>();
    auto previous = std::shared_ptr<const NotificationDestinations>(new NotificationDestinations,
        [&](const NotificationDestinations* value) {
            // Re-entering snapshot here detects destruction under its mutex.
            observed_replacement = routes.snapshot() == next;
            reclaimed = true;
            delete value;
        });
    routes.replace(std::move(previous));
    routes.replace(next);
    check(reclaimed && observed_replacement, "retirement must run outside lock after replacement");
}

void test_concurrent_snapshot_replacement() {
    NotificationRoutes routes;
    auto totals = std::make_shared<NotificationTotals>();
    auto first_destination = std::make_shared<NotificationDestination>(NotificationTarget{2, "first"}, totals);
    auto second_destination = std::make_shared<NotificationDestination>(NotificationTarget{3, "second"}, totals);
    auto first = std::make_shared<const NotificationDestinations>(NotificationDestinations{first_destination});
    auto second = std::make_shared<const NotificationDestinations>(NotificationDestinations{second_destination, second_destination});
    routes.replace(first);
    auto retained = routes.snapshot();
    std::promise<void> start;
    auto started = start.get_future().share();
    auto reading = std::async(std::launch::async, [&] {
        check(started.wait_for(test_timeout) == std::future_status::ready, "reader start timed out");
        for (std::size_t index = 0; index < replacement_count; ++index) {
            auto current = routes.snapshot();
            const bool complete_first = current == first && current->size() == 1 && current->front() == first_destination;
            const bool complete_second = current == second && current->size() == 2 && current->back() == second_destination;
            check(complete_first || complete_second, "reader observed a partial snapshot");
        }
    });
    start.set_value();
    for (std::size_t index = 0; index < replacement_count; ++index) {
        if (index % 2 == 0) {
            routes.replace(second);
        } else {
            routes.replace(first);
        }
    }
    reading.get();
    check(retained == first && retained->front() == first_destination, "replacement modified a retained snapshot");
}
}

void run_notification_snapshot_tests() {
    test_inflight_retirement_and_generation();
    test_reclamation_outside_lock();
    test_concurrent_snapshot_replacement();
}
}
