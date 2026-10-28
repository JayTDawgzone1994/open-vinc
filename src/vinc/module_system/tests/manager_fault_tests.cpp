#include "../../../../tests/support/manager.hpp"
// Use the actual supervisor and real child, with deterministic exception/exit checkpoints.
#include "../src/host/module_manager.cpp"
#include <iostream>

namespace {
using namespace vinc;
using Point = module_system::test::Point;
constexpr auto test_timeout = std::chrono::seconds(8);
constexpr auto retry_interval = std::chrono::milliseconds(10);
std::atomic<int> injection{-1};
std::atomic<bool> exit_confirmed{false};
std::atomic<bool> fail_notification{false};
std::mutex notification_mutex;
std::vector<std::string> ready_notifications;
std::string rejected_notification;
osal::NotificationReceiver conflicting_notification;
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}
void test_record_lifetime() {
    Record record;
    record.snapshot.mid = 1;
    record.snapshot.state = ModuleState::Running;
    const Error fault{module_system::ErrorCode::ObserveFailed, module_system::ErrorOperation::ObserveExit};
    record_failure(record, fault);
    const auto original_deadline = record.stop_deadline;
    for (int attempt = 0; attempt < 3; ++attempt) {
        record_failure(record, fault);
    }
    check(record.stop_deadline == original_deadline, "repeated failures must not postpone termination");
    record.pending_waiter = true;
    record.pending_request = 7;
    record.pending_generation = 1;
    record.response = control::Frame{control::Kind::Response, detail::commands, detail::execute, 7};
    {
        CommandWaiter waiter{record};
        retire_command(record);
        record.snapshot.generation = 2;
        retire_command(record);
        check(record.response && record.response->request == 7 && record.pending_generation == 1,
              "exit and generation reset preserve reply identity");
    }
    check(!record.pending_waiter && !record.response && !record.pending_request,
          "waiter exception cleanup discards completed result and releases busy");
    record.pending_request = 8;
    record.pending_waiter = true;
    {
        CommandWaiter waiter{record};
    }
    check(record.pending_request == 8 && !record.pending_waiter, "unknown outcome retains in-flight request");
    retire_command(record);
    check(!record.pending_request, "confirmed exit retires abandoned request");
}
}
namespace vinc::module_system::test {
Result<osal::TerminateResult> terminate_process(osal::Process& child) {
    return child.terminate();
}

void checkpoint(Point point, osal::Process* child, std::string_view resource) {
    if (point == Point::NotificationReserved && fail_notification.exchange(false)) {
        std::lock_guard lock(notification_mutex);
        rejected_notification = resource;
        conflicting_notification = take(osal::NotificationReceiver::create(resource));
    }
    if (point == Point::NotificationReady) {
        std::lock_guard lock(notification_mutex);
        ready_notifications.emplace_back(resource);
    }
    int expected = static_cast<int>(point);
    if (!injection.compare_exchange_strong(expected, -1)) {
        return;
    }
    if (point == Point::ResponseSaved) {
        auto requested = child->terminate();
        auto exited = child->wait_until(control::Clock::now() + test_timeout);
        exit_confirmed = requested && exited;
        return;
    }
    throw std::bad_alloc();
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "helper path required");
        test_record_lifetime();
        auto journal_probe = take(detail::Instance::create(vinc::test::instance_name()));
        const auto epochs = journal_probe->journal().parent_path();
        const auto epoch_digits = journal_probe->journal().filename().string().size();
        const auto journal_record = [&](const std::string& name) {
            return epochs / name.substr(1, epoch_digits) / name;
        };
        auto manager = take(vinc::test::create_manager());
        ModuleType type;
        type.name = "helper";

        type.library = std::filesystem::absolute(argv[1]);
        type.policy.max_restarts = 1;
        type.policy.restart_delay = std::chrono::milliseconds(100);
        check(bool(manager->register_module_type(type)), "register child");
        const auto mid = take(manager->start_module(type.name, {"--name", "fault-child"}));
        check(bool(manager->wait_running(mid, control::Clock::now() + test_timeout)), "child ready");
        // Fail a new Module's native create without disturbing the already running peer.
        ModuleType rejected_type = type;
        rejected_type.name = "notification-failure";
        rejected_type.policy.max_restarts = 0;
        check(bool(manager->register_module_type(rejected_type)), "register failure fixture");
        fail_notification = true;
        auto rejected_start = manager->start_module(rejected_type.name, {"--name", "rejected"});
        if (rejected_start) {
            auto rejected_ready = manager->wait_running(*rejected_start, control::Clock::now() + test_timeout);
            check(!rejected_ready, "notification setup failure reached Running");
        }
        // start_module may surface the launch failure directly; inspect retained diagnostics.
        auto snapshots = manager->snapshots();
        bool found_notification_failure = false;
        for (const auto& snapshot : snapshots) {
            if (snapshot.last_failure && snapshot.last_failure->error.is(module_system::ErrorCode::NotificationFailed)) {
                const auto& error = snapshot.last_failure->error;
                check(error.operation == static_cast<std::int32_t>(module_system::ErrorOperation::PrepareNotification),
                      "notification failure operation");
                check(!error.causes().empty() && error.causes().back().domain == ErrorDomain::Osal,
                      "notification failure lost OSAL cause");
                found_notification_failure = true;
            }
        }
        check(found_notification_failure, "missing notification failure diagnostic");
        check(bool(manager->command(mid, {}, control::Clock::now() + test_timeout)),
              "failed notification killed healthy peer");
        {
            std::lock_guard lock(notification_mutex);
            // Failed exclusive create must not unlink a pre-existing endpoint.
            auto preserved = take(osal::NotificationSender::open(rejected_notification));
            check(bool(preserved.notify()), "conflicting notification damaged");
            check(bool(conflicting_notification.close()), "close conflicting receiver");
            (void)osal::NotificationReceiver::unlink_name(rejected_notification);
        }
        for (const auto point : {Point::BeforeSend, Point::AfterSend}) {
            injection = static_cast<int>(point);
            bool threw = false;
            try {
                (void)manager->command(mid, {}, control::Clock::now() + test_timeout);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            check(threw, "allocation checkpoint reached");
            const auto deadline = control::Clock::now() + test_timeout;
            Result<std::int32_t> next = std::unexpected(Error{});
            do {
                next = manager->command(mid, {}, deadline);
                if (next || !next.error().is(module_system::ErrorCode::Busy)) {
                    break;
                }
                std::this_thread::sleep_for(retry_interval);
            } while (control::Clock::now() < deadline);
            check(next && *next == 42, "waiter cleanup and late reply release Busy");
        }
        injection = static_cast<int>(Point::ResponseSaved);
        auto final = manager->command(mid, {}, control::Clock::now() + test_timeout);
        check(exit_confirmed, "exit forced before supervisor continues same tick");
        check(final && *final == 42, "same-tick exit must not discard already received reply");
        const auto restart_deadline = control::Clock::now() + test_timeout;
        std::optional<ModuleSnapshot> restarted;
        do {
            restarted = manager->inspect("fault-child");
            if (restarted && restarted->mid != mid && restarted->state == ModuleState::Running) {
                break;
            }
            std::this_thread::sleep_for(retry_interval);
        } while (control::Clock::now() < restart_deadline);
        check(restarted && restarted->mid != mid && restarted->state == ModuleState::Running,
              "confirmed exit permits restart");
        {
            std::lock_guard lock(notification_mutex);
            check(ready_notifications.size() == 2 && ready_notifications[0] != ready_notifications[1],
                  "restart reused notification name");
            auto old = osal::NotificationReceiver::open(ready_notifications[0]);
            check(!old && old.error().is(osal::ErrorCode::NotFound), "dead generation notification retained");
            auto current = osal::NotificationSender::open(ready_notifications[1]);
            check(bool(current), "Ready closed the Host's notification resource");
        }
        auto next_generation = manager->command(restarted->mid, {"negative"}, restart_deadline);
        check(next_generation && *next_generation == -111, "old reply cannot leak into new generation");
        manager->begin_shutdown();
        check(bool(manager->wait_shutdown(control::Clock::now() + test_timeout)), "complete real child cleanup");
        {
            std::lock_guard lock(notification_mutex);
            check(!std::filesystem::exists(journal_record(rejected_notification)),
                  "failed create retained journal reservation after cleanup");
            for (const auto& name : ready_notifications) {
                check(!std::filesystem::exists(journal_record(name)), "stopped generation retained journal");
                auto reopened = osal::NotificationReceiver::open(name);
                check(!reopened && reopened.error().is(osal::ErrorCode::NotFound),
                      "shutdown retained notification resource");
            }
        }
        std::cout << "manager fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
