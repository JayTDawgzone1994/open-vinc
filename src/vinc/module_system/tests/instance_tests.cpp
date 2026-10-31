#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/instance.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/notification.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto timeout = 10s;
constexpr auto poll_interval = 5ms;
constexpr std::uint64_t payload_bytes = 4;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}
Deadline deadline() {
    return std::chrono::steady_clock::now() + timeout;
}

int child(const std::string& name, const std::filesystem::path& info) {
    auto instance = take(detail::Instance::create(name));
    const auto resource = take(instance->reserve());
    auto memory = take(osal::SharedMemory::create(resource, payload_bytes));
    auto mapping = take(memory.map(payload_bytes, osal::SharedMemoryAccess::ReadWrite));
    take(mapping.writable_bytes()).front() = std::byte{90};
    const auto notification_name = take(instance->reserve_notification());
    auto notification = take(osal::NotificationReceiver::create(notification_name));
    // A crash after reservation but before create must also be recoverable.
    const auto reserved_only = take(instance->reserve_notification());
    {
        std::ofstream output(info, std::ios::binary);
        output << resource << '\n' << take(osal::path_to_utf8(instance->journal())) << '\n'
               << notification_name << '\n' << reserved_only << '\n';
    }
    while (true) {
        std::this_thread::sleep_for(poll_interval);
    }
}

void run(const std::filesystem::path& executable) {
    const auto name = test::instance_name();
    auto other = take(detail::Instance::create(test::instance_name()));
    auto same_process = detail::Instance::create(other->name());
    check(!same_process && same_process.error().is(module_system::ErrorCode::InstanceRunning),
          "same-process duplicate central was admitted");
    const auto other_name = take(other->reserve());
    auto other_memory = take(osal::SharedMemory::create(other_name, payload_bytes));
    const auto other_notification_name = take(other->reserve_notification());
    auto other_notification = take(osal::NotificationReceiver::create(other_notification_name));
    const auto info = std::filesystem::temp_directory_path() / (name + ".txt");
    auto service = take(osal::ProcessService::create());
    osal::SpawnOptions options;
    options.executable = executable;
    options.arguments = {"child", name, take(osal::path_to_utf8(info))};
    auto process = take(service.spawn(options));
    struct Cleanup {
        osal::Process& process;
        std::filesystem::path info;
        ~Cleanup() {
            (void)process.terminate();
            (void)process.wait_until(deadline());
            std::error_code ignored;
            std::filesystem::remove(info, ignored);
        }
    } cleanup{process, info};

    std::string resource;
    std::string journal_text;
    std::string notification_name;
    std::string reserved_only;
    const auto ready_end = deadline();
    while (std::chrono::steady_clock::now() < ready_end) {
        std::ifstream input(info, std::ios::binary);
        if (std::getline(input, resource) && std::getline(input, journal_text) &&
            std::getline(input, notification_name) && std::getline(input, reserved_only)) {
            break;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    check(!resource.empty() && !journal_text.empty() && !reserved_only.empty(),
          "child did not publish resource records");
    const auto journal = take(osal::path_from_utf8(journal_text));
    auto old = take(osal::SharedMemory::open(resource, osal::SharedMemoryAccess::ReadWrite));
    auto view = take(old.map(payload_bytes, osal::SharedMemoryAccess::ReadWrite));
    auto old_receiver = take(osal::NotificationReceiver::open(notification_name));
    auto old_sender = take(osal::NotificationSender::open(notification_name));
    auto duplicate = detail::Instance::create(name);
    check(!duplicate && duplicate.error().is(module_system::ErrorCode::InstanceRunning),
          "live same-name central was admitted");
    check(std::filesystem::exists(journal / resource), "duplicate admission cleaned live journal");

    take(process.terminate());
    take(process.wait_until(deadline()));
    auto restarted = take(detail::Instance::create(name));
    check(!std::filesystem::exists(journal / resource), "dead central journal was not cleaned");
    check(!std::filesystem::exists(journal / notification_name), "notification journal not cleaned");
    check(!std::filesystem::exists(journal / reserved_only), "pre-create reservation not cleaned");
    const auto fresh_notification_name = take(restarted->reserve_notification());
    check(fresh_notification_name != notification_name, "notification name reused across generations");
    auto fresh_notification = take(osal::NotificationReceiver::create(fresh_notification_name));
    check(bool(old_sender.notify()), "old sender should still target its retained object");
    check(bool(old_receiver.wait_until(deadline())), "old object did not retain notification");
    auto empty = fresh_notification.wait_until(std::chrono::steady_clock::now());
    check(!empty && empty.error().is(osal::ErrorCode::Timeout), "old notification reached new generation");
    check(view.bytes().front() == std::byte{90}, "old mapping was rewritten on restart");
    const auto fresh_name = take(restarted->reserve());
    check(fresh_name != resource, "new central reused an old shared name");
    auto fresh = take(osal::SharedMemory::create(fresh_name, payload_bytes));
    auto fresh_view = take(fresh.map(payload_bytes, osal::SharedMemoryAccess::ReadWrite));
    check(fresh_view.bytes().front() == std::byte{0}, "new storage was not independent");
    check(std::filesystem::exists(other->journal() / other_name), "cleanup touched another Vinc name");
    check(std::filesystem::exists(other->journal() / other_notification_name),
          "cleanup touched another Vinc notification");
#ifndef _WIN32
    auto unlinked = osal::SharedMemory::open(resource, osal::SharedMemoryAccess::ReadOnly);
    check(!unlinked && unlinked.error().is(osal::ErrorCode::NotFound), "POSIX old name was not unlinked");
    auto delayed_host = osal::NotificationReceiver::open(notification_name);
    check(!delayed_host && delayed_host.error().is(osal::ErrorCode::NotFound),
          "delayed Host recreated retired notification");
#endif

    // Invalid journal contents must fail admission rather than delete arbitrary files.
    const auto unexpected = restarted->journal() / fresh_name / "unexpected";
    std::ofstream(unexpected) << "keep";
    restarted.reset();
    auto corrupt = detail::Instance::create(name);
    check(!corrupt && corrupt.error().is(module_system::ErrorCode::InvalidInstanceJournal),
          "invalid record accepted");
    check(std::filesystem::exists(unexpected), "invalid record deleted recursively");
    std::filesystem::remove(unexpected);
    restarted = take(detail::Instance::create(name));
    check(view.bytes().front() == std::byte{90}, "cleanup invalidated existing view");

    (void)osal::SharedMemory::unlink_name(other_name);
    check(bool(other->retire(other_name)), "retire other name");
    (void)osal::NotificationReceiver::unlink_name(other_notification_name);
    check(bool(other->retire(other_notification_name)), "retire other notification");
    check(!detail::Instance::create("../invalid"), "invalid instance name accepted");

    // Validate every kind before unlinking even the well-formed records in an epoch.
    const auto invalid_name = test::instance_name();
    auto invalid = take(detail::Instance::create(invalid_name));
    const auto valid_record = take(invalid->reserve_notification());
    auto unknown_kind = valid_record;
    unknown_kind.front() = 'x';
    const auto invalid_journal = invalid->journal();
    std::filesystem::create_directory(invalid_journal / unknown_kind);
    invalid.reset();
    auto refused = detail::Instance::create(invalid_name);
    check(!refused && refused.error().is(module_system::ErrorCode::InvalidInstanceJournal),
          "unknown resource kind accepted");
    check(std::filesystem::exists(invalid_journal / valid_record), "partial cleanup before full validation");
    std::filesystem::remove(invalid_journal / unknown_kind);
    invalid = take(detail::Instance::create(invalid_name));
    check(!std::filesystem::exists(invalid_journal / valid_record), "valid reservation not recovered");
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc == 4) {
            // CRT supplies native paths: wide on Windows, native bytes on POSIX.
            return child(take(vinc::osal::path_to_utf8(std::filesystem::path(argv[2]))),
                         std::filesystem::path(argv[3]));
        }
        check(argc == 1, "unexpected arguments");
        const auto name = test::instance_name();
        for (const auto& host : {std::filesystem::path{}, std::filesystem::path("relative-host")}) {
            auto refused = detail::ManagerAccess::create(name, {}, host);
            check(!refused && refused.error().is(module_system::ErrorCode::InvalidArgument) &&
                      refused.error().is_operation(module_system::ErrorOperation::Create),
                  "Manager requires an explicit absolute Host path");
        }

        // Invalid launch configuration must not reserve the central identity.
        auto admission = detail::Instance::create(name);
        check(bool(admission), "invalid Host path retained central identity");
        admission->reset();
        run(std::filesystem::absolute(argv[0]));
        std::cout << "PASS: instance admission, crash cleanup, names and retained mappings\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
