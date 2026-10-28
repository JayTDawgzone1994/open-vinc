#include <vinc/osal/notification.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/process.hpp>
#include <atomic>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace vinc::osal;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto operation_timeout = 5s;
constexpr auto timeout_probe = 30ms;
constexpr int lifecycle_iterations = 64;
constexpr int sender_threads = 4;
constexpr int flood_per_sender = 20000;
constexpr int maximum_drain_calls = sender_threads * flood_per_sender + 1;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}
void ok(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}
template<class T>
void fails(const vinc::Result<T>& result, ErrorCode code, ErrorOperation operation) {
    require(!result, "expected failure");
    require(result.error().is(code), "wrong error code");
    require(result.error().operation == static_cast<std::int32_t>(operation), "wrong operation");
    require(!vinc::error_to_string(result.error()).empty(), "missing diagnostic");
}
vinc::Deadline soon() {
    return Clock::now() + operation_timeout;
}
std::string unique_name() {
    static std::atomic_uint sequence{};
    static const auto salt = std::random_device{}();
    return "n" + std::to_string(salt) + "_" + std::to_string(++sequence);
}
struct Named {
    std::string name = unique_name();
    ~Named() {
#ifndef _WIN32
        (void)NotificationReceiver::unlink_name(name);
#endif
    }
};
void drain(NotificationReceiver& receiver) {
    for (int attempt = 0; attempt < maximum_drain_calls; ++attempt) {
        auto result = receiver.wait_until(Clock::now());
        if (!result) {
            fails(result, ErrorCode::Timeout, ErrorOperation::WaitNotification);
            return;
        }
    }
    throw std::runtime_error("notification backlog did not drain");
}

void basic() {
    for (const auto& [operation, text] : {
             std::pair{ErrorOperation::CreateNotification, "CreateNotification"},
             std::pair{ErrorOperation::OpenNotification, "OpenNotification"},
             std::pair{ErrorOperation::Notify, "Notify"},
             std::pair{ErrorOperation::WaitNotification, "WaitNotification"},
             std::pair{ErrorOperation::CloseNotification, "CloseNotification"},
             std::pair{ErrorOperation::UnlinkNotification, "UnlinkNotification"}}) {
        const vinc::Error error{ErrorCode::SystemError, operation};
        require(vinc::error_to_string(error).find(text) != std::string::npos,
                "notification operation missing from static error catalog");
    }
    for (const auto name : {"", "a/b", "a.b", "abcdefghijklmnopqrstuvwxyz"}) {
        fails(NotificationReceiver::create(name), ErrorCode::InvalidArgument,
              ErrorOperation::CreateNotification);
        fails(NotificationSender::open(name), ErrorCode::InvalidArgument,
              ErrorOperation::OpenNotification);
        fails(NotificationReceiver::open(name), ErrorCode::InvalidArgument,
              ErrorOperation::OpenNotification);
    }
    fails(NotificationReceiver::create(std::string_view("a\0b", 3)), ErrorCode::InvalidArgument,
          ErrorOperation::CreateNotification);
    fails(NotificationReceiver::open(std::string_view("a\0b", 3)), ErrorCode::InvalidArgument,
          ErrorOperation::OpenNotification);
    Named named;
    fails(NotificationSender::open(named.name), ErrorCode::NotFound, ErrorOperation::OpenNotification);
    fails(NotificationReceiver::open(named.name), ErrorCode::NotFound, ErrorOperation::OpenNotification);
    auto receiver = take(NotificationReceiver::create(named.name));
    auto sender = take(NotificationSender::open(named.name));
    ok(sender.notify());
    fails(NotificationReceiver::create(named.name), ErrorCode::AlreadyExists,
          ErrorOperation::CreateNotification);
    // Failed exclusive creation must not clear a notification that arrived first.
    ok(receiver.wait_until(Clock::now()));
    const auto start = Clock::now();
    fails(receiver.wait_until(start + timeout_probe), ErrorCode::Timeout, ErrorOperation::WaitNotification);
    require(Clock::now() >= start + timeout_probe, "timeout returned before deadline");

    auto moved_receiver = std::move(receiver);
    auto moved_sender = std::move(sender);
    fails(receiver.wait_until(soon()), ErrorCode::Closed, ErrorOperation::WaitNotification);
    fails(sender.notify(), ErrorCode::Closed, ErrorOperation::Notify);
    ok(moved_sender.notify());
    ok(moved_receiver.wait_until(soon()));
#ifdef _WIN32
    fails(NotificationReceiver::unlink_name(named.name), ErrorCode::Unsupported,
          ErrorOperation::UnlinkNotification);
#else
    ok(NotificationReceiver::unlink_name(named.name));
    fails(NotificationSender::open(named.name), ErrorCode::NotFound, ErrorOperation::OpenNotification);
    fails(NotificationReceiver::open(named.name), ErrorCode::NotFound, ErrorOperation::OpenNotification);
    // Unlink removes discovery, not the already-open endpoints.
    ok(moved_sender.notify());
    ok(moved_receiver.wait_until(soon()));
#endif
    ok(moved_receiver.close());
    ok(moved_receiver.close());
#ifdef _WIN32
    ok(moved_sender.notify()); // Remaining handle keeps the event alive.
#else
    fails(moved_sender.notify(), ErrorCode::PeerClosed, ErrorOperation::Notify);
#endif
    ok(moved_sender.close());
    ok(moved_sender.close());
    fails(moved_sender.notify(), ErrorCode::Closed, ErrorOperation::Notify);
}

void receiver_handoff() {
    Named named;
    auto creator = take(NotificationReceiver::create(named.name));
    auto sender = take(NotificationSender::open(named.name));
    ok(sender.notify());
    auto receiver = take(NotificationReceiver::open(named.name));
    // Opening and closing the creator must neither reset nor consume the hint.
    ok(creator.close());
    ok(receiver.wait_until(Clock::now()));
    fails(receiver.wait_until(Clock::now()), ErrorCode::Timeout, ErrorOperation::WaitNotification);
    auto moved = std::move(receiver);
    fails(receiver.wait_until(Clock::now()), ErrorCode::Closed, ErrorOperation::WaitNotification);
    ok(sender.notify());
    ok(moved.wait_until(soon()));
    ok(moved.close());
    ok(moved.close());
    ok(sender.close());
#ifndef _WIN32
    ok(NotificationReceiver::unlink_name(named.name));
#endif
    fails(NotificationReceiver::open(named.name), ErrorCode::NotFound, ErrorOperation::OpenNotification);
}

void cancellation() {
    Named named;
    auto creator = take(NotificationReceiver::create(named.name));
    auto receiver = take(NotificationReceiver::open(named.name));
    ok(creator.close());
    auto sender = take(NotificationSender::open(named.name));
    std::stop_source stopped;
    stopped.request_stop();
    fails(receiver.wait_until(soon(), stopped.get_token()), ErrorCode::Cancelled,
          ErrorOperation::WaitNotification);

    for (int iteration = 0; iteration < lifecycle_iterations; ++iteration) {
        std::atomic_bool entered = false;
        vinc::Result<void> waited;
        std::jthread waiter([&](std::stop_token stop) {
            entered = true;
            do {
                waited = receiver.wait_until((vinc::Deadline::max)(), stop);
            } while (!stop.stop_requested() && !waited && waited.error().is(ErrorCode::Busy));
        });
        // Probe Busy with immediate waits until the other waiter owns admission.
        // jthread cancellation/join also makes failure cleanup safe.
        bool saw_busy = false;
        const auto deadline = soon();
        while (Clock::now() < deadline) {
            if (entered) {
                auto probe = receiver.wait_until(Clock::now());
                if (!probe && probe.error().is(ErrorCode::Busy)) {
                    saw_busy = true;
                    break;
                }
            }
            std::this_thread::yield();
        }
        require(saw_busy, "second waiter did not observe Busy");
        waiter.request_stop();
        waiter.join();
        fails(waited, ErrorCode::Cancelled, ErrorOperation::WaitNotification);
        drain(receiver); // Cancellation is allowed to leave a hint behind.
        ok(sender.notify());
        ok(receiver.wait_until(soon()));
    }
}

void saturation() {
    Named named;
    auto receiver = take(NotificationReceiver::create(named.name));
    auto sender = take(NotificationSender::open(named.name));
    std::atomic_bool failed = false;
    std::vector<std::jthread> workers;
    for (int index = 0; index < sender_threads; ++index) {
        workers.emplace_back([&] {
            for (int count = 0; count < flood_per_sender; ++count) {
                if (!sender.notify()) {
                    failed = true;
                }
            }
        });
    }
    workers.clear(); // No consumer runs until every producer has returned.
    require(!failed, "flood notification failed");
    ok(receiver.wait_until(Clock::now()));
    drain(receiver);
    ok(sender.notify());
    ok(receiver.wait_until(soon()));
}

struct Child {
    Process process;
    ~Child() {
        if (process) {
            (void)process.terminate();
            (void)process.wait_until(soon());
        }
    }
    void finish() {
        auto status = take(process.wait_until(soon()));
        require(status.kind == ExitKind::Code && status.value == 0, "child failed");
        process = {};
    }
};
Child spawn(ProcessService& service, const std::filesystem::path& executable,
            std::vector<std::string> arguments) {
    SpawnOptions options;
    options.executable = executable;
    options.arguments = std::move(arguments);
    return Child{take(service.spawn(options))};
}
void cross_process(const std::filesystem::path& executable) {
    auto service = take(ProcessService::create());
    Named ready_name;
    Named command_name;
    auto ready = take(NotificationReceiver::create(ready_name.name));
    auto child = spawn(service, executable, {"child", "exchange", ready_name.name, command_name.name});
    ok(ready.wait_until(soon()));
    auto command = take(NotificationSender::open(command_name.name));
    ok(command.notify());
    ok(ready.wait_until(soon()));
    child.finish();

    Named handoff_name;
    auto creator = take(NotificationReceiver::create(handoff_name.name));
    auto handoff_sender = take(NotificationSender::open(handoff_name.name));
    ok(handoff_sender.notify());
    auto receiver_child = spawn(service, executable, {"child", "handoff", ready_name.name, handoff_name.name});
    ok(ready.wait_until(soon())); // Child opened and consumed the pre-open hint.
    ok(creator.close());
    ok(handoff_sender.notify());
    receiver_child.finish(); // Child received again after the creator closed.

    // Kill an actual receiver process, then prove the sender survives notify.
    Named crash_name;
    auto crash = spawn(service, executable, {"child", "crash", ready_name.name, crash_name.name});
    ok(ready.wait_until(soon()));
    auto survivor = take(NotificationSender::open(crash_name.name));
    (void)take(crash.process.terminate());
    (void)take(crash.process.wait_until(soon()));
    crash.process = {};
#ifdef _WIN32
    ok(survivor.notify());
#else
    fails(survivor.notify(), ErrorCode::PeerClosed, ErrorOperation::Notify);
    fails(NotificationReceiver::create(crash_name.name), ErrorCode::AlreadyExists,
          ErrorOperation::CreateNotification);
    ok(NotificationReceiver::unlink_name(crash_name.name));
#endif
    ok(survivor.close());
    auto replacement = take(NotificationReceiver::create(crash_name.name));
    auto replacement_sender = take(NotificationSender::open(crash_name.name));
    ok(replacement_sender.notify());
    ok(replacement.wait_until(soon()));

    // Each child opens its own sender; finish each bounded child without making
    // notification counts part of the contract.
    for (int index = 0; index < sender_threads; ++index) {
        auto producer = spawn(service, executable, {"child", "signal", ready_name.name, "unused"});
        producer.finish();
    }
    ok(ready.wait_until(soon()));
    drain(ready);
}
void lifecycle() {
#ifdef _WIN32
    DWORD before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count failed");
#endif
    for (int iteration = 0; iteration < lifecycle_iterations; ++iteration) {
        Named named;
        auto receiver = take(NotificationReceiver::create(named.name));
        auto opened = take(NotificationReceiver::open(named.name));
        auto sender = take(NotificationSender::open(named.name));
        ok(receiver.close());
        ok(sender.notify());
        ok(opened.wait_until(soon()));
    }
#ifdef _WIN32
    DWORD after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "handle count failed");
    if (after != before) {
        std::cerr << "notification handle baseline=" << before << ", after=" << after << '\n';
    }
    require(after == before, "notification leaked Windows handles");
#endif
}

#ifdef _WIN32
void native_creation_failure() {
    Named named;
    const auto event_name = L"Local\\ov_notify_" + std::wstring(named.name.begin(), named.name.end());
    struct MutexOwner {
        HANDLE handle;
        ~MutexOwner() {
            CloseHandle(handle);
        }
    } mutex{CreateMutexW(nullptr, FALSE, event_name.c_str())};
    require(mutex.handle != nullptr, "create conflicting native object");
    DWORD before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "native failure handle baseline");
    for (int iteration = 0; iteration < lifecycle_iterations; ++iteration) {
        auto created = NotificationReceiver::create(named.name);
        fails(created, ErrorCode::InvalidArgument, ErrorOperation::CreateNotification);
        require(created.error().native.code == ERROR_INVALID_HANDLE, "preserve CreateEvent native error");
        fails(NotificationSender::open(named.name), ErrorCode::InvalidArgument,
              ErrorOperation::OpenNotification);
        fails(NotificationReceiver::open(named.name), ErrorCode::InvalidArgument,
              ErrorOperation::OpenNotification);
    }
    DWORD after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "native failure handle result");
    require(after == before, "failed creation leaked native handles");
}
#endif
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "child") {
            const auto mode = std::string_view(argv[2]);
            auto ready = take(NotificationSender::open(argv[3]));
            if (mode == "signal") {
                ok(ready.notify());
                return 0;
            }
            if (mode == "handoff") {
                auto receiver = take(NotificationReceiver::open(argv[4]));
                ok(receiver.wait_until(soon()));
                ok(ready.notify());
                ok(receiver.wait_until(soon()));
                return 0;
            }
            auto command = take(NotificationReceiver::create(argv[4]));
            ok(ready.notify());
            ok(command.wait_until(soon()));
            require(mode == "exchange", "crash helper unexpectedly woke");
            ok(ready.notify());
            return 0;
        }
        require(argc == 2, "expected own executable path");
        basic();
        receiver_handoff();
        // Count handles before ProcessService creates asynchronous reapers.
        lifecycle();
#ifdef _WIN32
        native_creation_failure();
#endif
        cancellation();
        saturation();
        cross_process(take(path_from_utf8(argv[1])));
        std::cout << "PASS: notification contract, cancellation, saturation, process death, lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
