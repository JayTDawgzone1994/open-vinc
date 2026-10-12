#include "../../../../tests/support/manager.hpp"
#include <vinc/bag/bag.hpp>
#include <vinc/osal/path.hpp>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto operation_budget = 20s;
constexpr auto observe_interval = 50ms;
constexpr auto discovery_grace = 1200ms;
std::function<void()> diagnostics;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

template <class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + operation_budget;
    while (!predicate()) {
        if (diagnostics) {
            diagnostics();
        }

        check(std::chrono::steady_clock::now() < deadline, "bounded module observation");
        std::this_thread::sleep_for(observe_interval);
    }
}

void command(ModuleManager& manager, const std::string& name, std::vector<std::string> args) {
    std::cout << "command " << name << " " << args.front() << '\n';
    check(take(manager.command(name, std::move(args), std::chrono::steady_clock::now() + operation_budget)) ==
              0,
          "module command succeeds");
}

void start(ModuleManager& manager, const std::string& type, std::vector<std::string> args) {
    const auto mid = take(manager.start_module(type, std::move(args)));
    take(manager.wait_started(mid));
}

void stop(ModuleManager& manager, const std::string& name) {
    take(manager.stop_module(name, std::chrono::steady_clock::now() + operation_budget));
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 4, "plugin arguments");
        auto manager = take(vinc::test::create_manager());
        auto output = take(manager->output_hub().subscribe());
        std::string trace;
        auto drain_output = [&] {
            for (const auto& event : output.take().events) {
                trace += event->bytes;
                std::cout << event->bytes;
            }
        };
        diagnostics = drain_output;
        ModuleType recorder{"bagRecorder", std::filesystem::absolute(argv[1]), "recorder", {}};
        recorder.policy.stop_timeout = operation_budget;
        take(manager->register_module_type(recorder));
        take(manager->register_module_type(
            {"messageBusTest", std::filesystem::absolute(argv[2]), "publisher", {}}));
        const auto root = std::filesystem::temp_directory_path() / vinc::test::instance_name();
        check(std::filesystem::create_directory(root), "exclusive test directory");
        auto path_text = [&](const char* name) {
            return take(osal::path_to_utf8(root / name));
        };
        start(*manager, "bagRecorder", {"record", path_text("known"), "--all"});
        start(*manager, "messageBusTest", {"--name", "first", "pub", "camera/data", "8", "8192"});
        start(*manager, "messageBusTest", {"--name", "second", "pub", "camera/data", "8", "8192"});
        std::this_thread::sleep_for(discovery_grace);
        command(*manager, "first", {"push", "1", "64"});
        command(*manager, "second", {"push", "2", "64"});
        until([&] {
            auto info = bag::inspect(root / "known");
            return info && info->counters.written >= 2;
        });
        stop(*manager, "first");
        start(*manager, "messageBusTest", {"--name", "first", "pub", "camera/data", "8", "8192"});
        std::this_thread::sleep_for(discovery_grace);
        command(*manager, "first", {"push", "3", "64"});
        until([&] {
            auto info = bag::inspect(root / "known");
            return info && info->sources.size() == 3;
        });
        command(*manager, "recorder", {"finish"});
        until([&] {
            auto info = bag::inspect(root / "known");
            return info && info->completed;
        });
        command(*manager, "recorder", {"info", path_text("known")});
        auto known = take(bag::inspect(root / "known"));
        check(known.counters.written == 3 && known.topics.size() == 1, "all publishes recorded exactly once");
        check(known.sources.size() == 3 &&
                  known.sources.front().publisher_id != known.sources.back().publisher_id,
              "restart preserves source identity");
        command(*manager, "recorder",
                {"record", path_text("bounded"), "--topic", "camera/data", "--message-bytes", "2048"});
        std::this_thread::sleep_for(discovery_grace);
        command(*manager, "first", {"push", "4", "4096"});
        until([&] {
            auto info = bag::inspect(root / "bounded");
            return info && info->counters.oversized == 1;
        });
        command(*manager, "first", {"push", "5", "64"});
        until([&] {
            auto info = bag::inspect(root / "bounded");
            return info && info->counters.written == 1;
        });
        stop(*manager, "recorder");
        check(take(bag::inspect(root / "bounded")).completed, "normal module stop drains queue");
        start(*manager, "bagRecorder", {"record", path_text("fault"), "--all"});
        until([&] {
            auto info = bag::inspect(root / "fault");
            return info && !info->topics.empty();
        });
        sqlite3* blocker = nullptr;
        const auto blocked_file = take(osal::path_to_utf8(root / "fault" / "recording.sqlite"));
        check(sqlite3_open(blocked_file.c_str(), &blocker) == SQLITE_OK, "open disk-contention fixture");
        sqlite3_busy_timeout(blocker, vinc::config::bag_sqlite_busy_timeout_ms);
        check(sqlite3_exec(blocker, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK,
              "hold writer beyond its contention budget");
        command(*manager, "first", {"push", "7", "64"});
        until([&] {
            drain_output();
            return trace.find("recording stopped:") != std::string::npos;
        });
        sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(blocker);
        check(!take(bag::inspect(root / "fault")).completed, "fault stays incomplete");
        command(*manager, "recorder", {"record", path_text("after-fault"), "--topic", "camera/data"});
        std::this_thread::sleep_for(discovery_grace);
        command(*manager, "first", {"push", "8", "64"});
        until([&] {
            auto info = bag::inspect(root / "after-fault");
            return info && info->counters.written == 1;
        });
        stop(*manager, "recorder");
        check(take(bag::inspect(root / "after-fault")).completed,
              "same Module recovers with a new recording");
        // Exercise the CLI limit through the real Plugin and shared-memory Bus.
        constexpr std::uint64_t payload_bytes = 1024 * 1024;
        constexpr std::uint64_t database_bytes = 4 * payload_bytes;
        start(*manager, "messageBusTest",
              {"--name", "largePublisher", "pub", "camera/quota", "2", std::to_string(2 * payload_bytes)});
        start(*manager, "bagRecorder",
              {"record", path_text("quota"), "--topic", "camera/quota", "--database-bytes",
               std::to_string(database_bytes)});
        until([&] {
            auto info = bag::inspect(root / "quota");
            return info && !info->topics.empty();
        });
        const auto trace_start = trace.size();
        for (int sequence = 1; sequence <= 4; ++sequence) {
            command(*manager, "largePublisher",
                    {"push", std::to_string(sequence), std::to_string(payload_bytes)});
            until([&] {
                auto info = bag::inspect(root / "quota");
                return (info && info->counters.written >= static_cast<std::uint64_t>(sequence)) ||
                       trace.find("recording stopped:", trace_start) != std::string::npos;
            });
        }
        check(trace.find("recording stopped:", trace_start) != std::string::npos,
              "quota failure reported through Module output");
        const auto quota = take(bag::inspect(root / "quota"));
        check(!quota.completed && quota.counters.written == 3, "quota preserves prior committed messages");
        check(std::filesystem::file_size(root / "quota" / "recording.sqlite") <= database_bytes,
              "Module database obeys CLI bound");
        command(*manager, "recorder", {"status"});
        command(*manager, "first", {"push", "9", "64"});
        command(*manager, "recorder", {"record", path_text("after-quota"), "--all"});
        command(*manager, "recorder", {"finish"});
        until([&] {
            auto info = bag::inspect(root / "after-quota");
            return info && info->completed;
        });
        stop(*manager, "recorder");
        stop(*manager, "largePublisher");
        take(manager->unregister_module_type("bagRecorder"));
        recorder.library = std::filesystem::absolute(argv[3]);
        take(manager->register_module_type(recorder));
        start(*manager, "bagRecorder", {"record", path_text("unknown"), "--all"});
        until([&] {
            auto info = bag::inspect(root / "unknown");
            return info && info->topics.size() == 1;
        });
        command(*manager, "first", {"push", "6", "64"});
        command(*manager, "recorder", {"status"});
        stop(*manager, "recorder");
        auto unknown = take(bag::inspect(root / "unknown"));
        check(unknown.completed && unknown.counters.written == 0 && !unknown.topics.front().topic.supported,
              "unknown type preserves metadata only");
        start(*manager, "bagRecorder", {});
        command(*manager, "recorder", {"info", path_text("known")});
        until([&] {
            drain_output();
            return trace.find("unsupported type") != std::string::npos &&
                   trace.find("unknown type") != std::string::npos;
        });
        sqlite3* database = nullptr;
        const auto filename = take(osal::path_to_utf8(root / "known" / "recording.sqlite"));
        check(sqlite3_open(filename.c_str(), &database) == SQLITE_OK, "open schema metadata fixture");
        check(sqlite3_exec(database, "UPDATE topics SET type='test'", nullptr, nullptr, nullptr) == SQLITE_OK,
              "simulate a different version of a locally named type");
        sqlite3_close(database);
        command(*manager, "recorder", {"info", path_text("known")});
        until([&] {
            drain_output();
            return trace.find("schema mismatch") != std::string::npos;
        });
        manager->begin_shutdown();
        take(manager->wait_shutdown(std::chrono::steady_clock::now() + operation_budget));
        std::cout << "bag module passed: " << root.string() << '\n';
        check(std::filesystem::weakly_canonical(root).parent_path() ==
                  std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()),
              "cleanup boundary");
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
