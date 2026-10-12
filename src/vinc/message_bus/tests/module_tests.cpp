#include "../../../../tests/support/manager.hpp"
// End-to-end Message Bus tests through independent Module processes and the central BusManager.
// Verify multiple Publishers, copied reads, loans, topology changes and crash cleanup.

#include <vinc/module/host/module_manager.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>
using namespace vinc;
using namespace std::chrono_literals;

namespace {
constexpr auto heartbeat_interval = 100ms;
constexpr auto heartbeat_timeout = 1500ms;
constexpr auto startup_timeout = 15s;
constexpr auto running_timeout = 18s;
constexpr auto command_timeout = 10s;
constexpr auto topology_timeout = 5s;
constexpr auto topology_retry_interval = 10ms;
constexpr auto crash_command_timeout = 2s;
constexpr auto blocked_writer_probe = 1700ms; // Deliberately longer than heartbeat_timeout.
constexpr auto shutdown_timeout = 5s;
// Helper wire return codes are independent expectations, not derived from ErrorCode.
constexpr int no_new_data = -101;
constexpr int no_published_data = -102;
constexpr int outstanding_loan = -103;
constexpr int capacity_exceeded = -106;
constexpr int type_mismatch = -107;
}

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        auto manager_owner = vinc::test::create_manager();
        if (!manager_owner) {
            throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
        }
        auto& manager = **manager_owner;
        ModulePolicy policy;
        policy.heartbeat_interval = heartbeat_interval;
        policy.heartbeat_timeout = heartbeat_timeout;
        policy.startup_timeout = startup_timeout;
        check(bool(manager.register_module_type(
                  {"messageBusTest", std::filesystem::absolute(argv[1]), "messageBusNode", policy})),
              "register helper");
        unsigned instance = 0;
        auto start = [&](std::vector<std::string> args) {
            args.insert(args.begin(), {"--name", "messageBusFixture" + std::to_string(++instance)});
            auto name = manager.start_module("messageBusTest", std::move(args));
            check(bool(name), "start helper");
            auto result = manager.wait_running(*name, control::Clock::now() + running_timeout);
            if (!result) {
                const auto snapshot = manager.inspect(*name);
                throw std::runtime_error(vinc::error_to_string(result.error()));
            }

            return manager.inspect(*name)->name;
        };
        auto command = [&](const std::string& module, std::vector<std::string> args) {
            auto result = manager.command(module, std::move(args), control::Clock::now() + command_timeout);
            if (!result) {
                throw std::runtime_error(vinc::error_to_string(result.error()));
            }

            return *result;
        };
        auto eventually = [&](auto predicate) {
            const auto end = control::Clock::now() + topology_timeout;
            while (control::Clock::now() < end) {
                if (predicate()) {
                    return true;
                }

                std::this_thread::sleep_for(topology_retry_interval);
            }

            return predicate();
        };
        // Scenario: registration before any Publisher and the copied publication path.
        auto subscriber = start({"sub", "camera/frames"});
        check(command(subscriber, {"last"}) == no_published_data,
              "get_last before Publisher fails immediately");
        check(command(subscriber, {"next"}) == no_new_data, "next before Publisher fails immediately");
        auto primary_publisher = start({"pub", "camera/frames", "1", "4194304"});
        check(command(primary_publisher, {"slots"}) == 2, "one requested slot becomes two");
        check(command(primary_publisher, {"wrong-schema"}) == type_mismatch,
              "topic schema mismatch rejected");
        check(command(primary_publisher, {"push", "1"}) == 0, "local push");
        check(eventually([&] {
                  return command(subscriber, {"last"}) == 1;
              }),
              "first shared message");
        check(command(subscriber, {"last"}) == 1, "latest repeats");
        check(command(subscriber, {"publisher-is", primary_publisher}) == 1,
              "publisher metadata must identify the Module, not the topic prefix");
        check(command(subscriber, {"next"}) == 1, "latest did not advance next");
        check(command(subscriber, {"next"}) == no_new_data, "no new message");
        // Scenario: a large nested loan is invisible until committed.
        check(command(primary_publisher, {"loan", "2", "2097152"}) == 0, "shared loan with large payload");
        check(command(subscriber, {"last"}) == 1, "last remains visible during loan");
        check(command(primary_publisher, {"loan", "99"}) == outstanding_loan, "second loan fails");
        check(command(primary_publisher, {"push", "99"}) == outstanding_loan,
              "push cannot bypass outstanding loan");
        check(command(primary_publisher, {"commit"}) == 0, "commit shared message");
        check(command(subscriber, {"next"}) == 2, "large nested message received");
        check(command(subscriber, {"copy-size"}) == 2097152, "full payload copied");
        // Scenario: independent next cursors and latest reads across two Publishers.
        auto history_subscriber = start({"sub", "camera/frames"});
        check(command(history_subscriber, {"next"}) == 1, "default subscription starts oldest retained");
        auto new_only_subscriber = start({"sub", "camera/frames", "new"});
        check(command(new_only_subscriber, {"next"}) == no_new_data,
              "new-only cursor starts at subscription");
        check(command(new_only_subscriber, {"last"}) == 2, "latest is independent of new-only cursor");
        auto secondary_publisher = start({"pub", "camera/frames", "3", "8192"});
        check(command(secondary_publisher, {"push", "3"}) == 0, "second Publisher");
        check(eventually([&] {
                  return command(subscriber, {"last"}) == 3;
              }),
              "dynamic Publisher attachment");
        check(command(subscriber, {"publisher-is", secondary_publisher}) == 1,
              "shared topic must preserve the second Module publisher identity");
        check(command(primary_publisher, {"push", "4"}) == 0, "publish after second Publisher");
        check(command(subscriber, {"next"}) == 3 && command(subscriber, {"next"}) == 4,
              "merge visible candidates by timestamp");
        check(command(secondary_publisher, {"push", "5", "9000"}) == capacity_exceeded, "oversize fails");
        check(command(secondary_publisher, {"published"}) == 1 && command(subscriber, {"last"}) == 4,
              "failed push preserves publication history");
        check(command(primary_publisher, {"push", "5"}) == 0 &&
                  command(primary_publisher, {"push", "6"}) == 0,
              "overwrite oldest history");
        check(command(history_subscriber, {"next"}) == 3 && command(history_subscriber, {"next"}) == 5 &&
                  command(history_subscriber, {"next"}) == 6,
              "slow next skips only overwritten entries");
        check(command(primary_publisher, {"loan", "7"}) == 0 && command(primary_publisher, {"drop"}) == 0,
              "discard outstanding object");
        check(command(primary_publisher, {"loan", "8"}) == outstanding_loan,
              "dropped loan remains outstanding");
        check(command(subscriber, {"last"}) == 6, "abandoned loan leaves last published message");
        // Scenario: Publisher death retires history but preserves received local copies.
        (void)manager.command(primary_publisher, {"crash"}, control::Clock::now() + crash_command_timeout);
        check(eventually([&] {
                  return manager.inspect(primary_publisher)->state == ModuleState::Failed;
              }),
              "Publisher death detected");
        check(eventually([&] {
                  return command(subscriber, {"last"}) == 3;
              }),
              "dead Publisher history retired");
        check(command(history_subscriber, {"copy-value"}) == 6, "received copy survives Publisher death");
        check(command(secondary_publisher, {"push", "9"}) == 0, "other Publisher survives");
        check(command(subscriber, {"next"}) == 9, "subscription survives Publisher death");
        check(command(secondary_publisher, {"close"}) == 0, "normal Publisher close");
        check(eventually([&] {
                  return command(subscriber, {"last"}) == no_published_data;
              }),
              "last Publisher removal gives no data");
        auto restarted = start({"pub", "camera/frames", "2", "8192"});
        check(command(restarted, {"push", "10"}) == 0, "new Publisher instance");
        check(eventually([&] {
                  return command(subscriber, {"next"}) == 10;
              }),
              "new instance has independent cursor");
        // Scenario: heartbeat survives a blocked copy; confirmed reader death releases it.
        auto pinned_reader = start({"sub", "fault"});
        auto blocked_writer = start({"pub", "fault", "2", "8192"});
        check(command(blocked_writer, {"push", "20"}) == 0, "fault setup first message");
        check(eventually([&] {
                  return command(pinned_reader, {"last"}) == 20;
              }),
              "reader attachment before pin");
        check(command(pinned_reader, {"pin"}) == 0, "hold reader at copy boundary");
        check(command(blocked_writer, {"push", "21"}) == 0, "other slot remains writable");
        auto waiting = std::async(std::launch::async, [&] {
            return command(blocked_writer, {"push", "22"});
        });
        check(waiting.wait_for(blocked_writer_probe) == std::future_status::timeout,
              "collision waits past heartbeat timeout");
        check(manager.inspect(blocked_writer)->state == ModuleState::Running &&
                  manager.inspect(pinned_reader)->state == ModuleState::Running,
              "heartbeats survive blocked writer and pinned reader");
        (void)manager.command(pinned_reader, {"crash"}, control::Clock::now() + crash_command_timeout);
        check(waiting.get() == 0, "confirmed reader death releases writer");
        check(command(blocked_writer, {"collisions"}) == 1 && command(blocked_writer, {"waited"}) == 1,
              "collision count and duration recorded");
        check(bool(manager.stop_module(blocked_writer, control::Clock::now() + shutdown_timeout)),
              "stop recovered writer");
        for (const auto& module :
             {subscriber, history_subscriber, new_only_subscriber, secondary_publisher, restarted}) {
            check(bool(manager.stop_module(module, control::Clock::now() + shutdown_timeout)), "stop helper");
        }

        std::cout << "message_bus module integration passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
