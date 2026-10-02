// Real processes contend over mapped payloads and die while holding the lock.
// Pipe handshakes distinguish a held lock from a child that has not started yet.
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace vinc::osal;
using vinc::concurrency::InterprocessMutex;
using namespace std::chrono_literals;

// Test budgets and workload sizes are kept separate from production lock policy.
constexpr auto operation_timeout = 10s;
constexpr auto external_lock_timeout = 60ms;
constexpr auto file_lock_timeout = 30ms;
constexpr auto blocked_wait_observation = 20ms;
constexpr auto cancellation_timeout = 2s;
constexpr int local_worker_count = 4;
constexpr int local_increments_per_worker = 100;
constexpr int child_worker_count = 3;
constexpr int increments_per_process = 200;
constexpr std::size_t snapshot_value_count = 64;
constexpr int abrupt_exit_status = 17;
constexpr int unexpected_kill_resume_status = 99;
// One-byte fixture protocol: ready, start, completed, and permission to exit.
constexpr std::byte ready_token{1};
constexpr std::byte start_token{2};
constexpr std::byte completed_token{3};
constexpr std::byte exit_token{4};

std::string describe(const OsError& failure) {
    return failure.native.message();
}

std::string describe(const vinc::concurrency::Error& failure) {
    return failure.os_error ? failure.os_error->native.message() : std::string{};
}

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

template <class T, class E> T take(std::expected<T, E> result) {
    if (!result) {
        throw std::runtime_error("OSAL code=" + std::to_string(static_cast<int>(result.error().code)) + " " +
                                 describe(result.error()));
    }
    return std::move(*result);
}

template <class E> void ok(std::expected<void, E> result) {
    if (!result) {
        throw std::runtime_error(describe(result.error()));
    }
}

template <class T, class E> void fails(const std::expected<T, E>& result, ErrorCode code, const char* text) {
    check(!result && result.error().code == code, text);
}

Deadline test_deadline() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    auto encoded_path = path.u8string();
    // Native path boundary: char may read the char8_t object representation;
    // copying preserves the UTF-8 bytes and does not retain the temporary storage.
    return {reinterpret_cast<const char*>(encoded_path.data()), encoded_path.size()};
}

std::filesystem::path path_from_utf8(const std::string& text) {
    // Legacy UTF-8 path boundary assumes char bytes can be read as char8_t code units.
    // TODO: review this aliasing assumption separately from the readability changes.
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void send_token(PipeService& pipes, PipeWriter& writer, std::byte token) {
    IoBuffer buffer(1);
    buffer.bytes()[0] = token;
    auto id = take(pipes.submit_write(writer, std::move(buffer), test_deadline()));
    std::array<IoCompletion, 1> completions;
    check(take(pipes.wait_completions(completions, test_deadline())) == 1 && completions[0].id == id &&
              completions[0].status == IoStatus::Completed && completions[0].transferred == 1,
          "send token");
}

void receive_token(PipeService& pipes, PipeReader& reader, std::byte token) {
    auto id = take(pipes.submit_read(reader, IoBuffer(1), test_deadline()));
    std::array<IoCompletion, 1> completions;
    check(take(pipes.wait_completions(completions, test_deadline())) == 1 && completions[0].id == id &&
              completions[0].status == IoStatus::Completed && completions[0].transferred == 1 &&
              completions[0].buffer.bytes()[0] == token,
          "receive token");
}

struct Payload {
    SharedWord active{};
    std::uint64_t count = 0;
    std::array<std::uint64_t, snapshot_value_count> values{};
};

void increment(InterprocessMutex& mutex, Payload& payload) {
    auto guard = take(mutex.lock_until(test_deadline()));
    check(shared_fetch_add(payload.active, 1) == 0, "overlapping critical sections");
    for (auto value : payload.values) {
        check(value == payload.count, "torn shared snapshot");
    }
    const auto next = payload.count + 1;
    for (auto& value : payload.values) {
        value = next;
        std::this_thread::yield();
    }
    payload.count = next;
    check(shared_fetch_add(payload.active, std::uint64_t(-1)) == 1, "active owner mismatch");
    ok(guard.unlock());
}

int child(const std::vector<std::string>& args) {
    auto pipes = take(PipeService::create());
    auto pipe = take(take_inherited_pipe(pipes));
    auto mutex = take(InterprocessMutex::open(path_from_utf8(args.at(2))));
    const auto& mode = args.at(3);
    if (mode == "abrupt" || mode == "kill") {
        auto guard = take(mutex.lock_until(test_deadline()));
        mutex.close(); // The guard alone must retain the native handle.
        send_token(pipes, pipe.writer, ready_token);
        receive_token(pipes, pipe.reader, start_token);
        if (mode == "abrupt") {
            std::_Exit(abrupt_exit_status); // Skip all C++ cleanup.
        }
        return unexpected_kill_resume_status; // kill test never sends this token
    }

    auto memory = take(SharedMemory::open(args.at(4), SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    // Shared-memory ABI boundary: parent and child are the same executable/layout.
    // The mapping starts at offset zero with OS page alignment and sizeof(Payload)
    // bytes. Parent constructs Payload before spawn and retains it until children exit.
    auto* payload = reinterpret_cast<Payload*>(take(mapping.writable_bytes()).data());
    send_token(pipes, pipe.writer, ready_token);
    receive_token(pipes, pipe.reader, start_token);
    for (int i = 0; i < increments_per_process; ++i) {
        increment(mutex, *payload);
    }
    send_token(pipes, pipe.writer, completed_token);
    receive_token(pipes, pipe.reader, exit_token);
    pipe.close();
    ok(pipes.shutdown_until(test_deadline()));
    return 0;
}

struct Workspace {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::current_path());
    std::filesystem::path path = root / ("mutex-test-" + std::to_string(std::random_device{}()));

    Workspace() {
        check(std::filesystem::create_directory(path), "create private test directory");
#ifndef _WIN32
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
#endif
    }

    ~Workspace() {
        if (path.parent_path() == root) {
            std::error_code failure;
            std::filesystem::remove_all(path, failure);
        }
    }
};

struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes = take(PipeService::create());
    std::filesystem::path executable;
    std::vector<ProcessId> children;

    explicit Harness(std::filesystem::path exe) : executable(std::move(exe)) {
    }

    ~Harness() {
        for (auto id : children) {
            (void)processes.terminate(id);
        }
        (void)pipes.shutdown_until(test_deadline());
        (void)processes.shutdown_until(test_deadline());
    }

    ProcessWithPipe spawn(const std::filesystem::path& lock, const std::string& mode,
                          const std::string& memory = "") {
        SpawnOptions options;
        options.executable = executable;
        options.arguments = {"--child", path_to_utf8(lock), mode, memory};
        auto child = take(spawn_with_pipe(processes, pipes, options));
        children.push_back(child.process.id());
        return child;
    }

    ExitStatus wait(ProcessWithPipe& child_process) {
        auto result = take(child_process.process.wait_until(test_deadline()));
        child_process.pipe.close();
        ok(processes.forget(child_process.process.id()));
        return result;
    }
};

void test_ownership_and_external_wait(InterprocessMutex& mutex, InterprocessMutex& second,
                                      const std::filesystem::path& path) {
    auto guard = take(mutex.try_lock());
    fails(mutex.try_lock(), ErrorCode::Busy, "nonrecursive same handle");
    fails(second.try_lock(), ErrorCode::WouldBlock, "separate handle must contend");

    auto another = std::async(std::launch::async, [&] {
        return mutex.try_lock();
    });
    fails(another.get(), ErrorCode::WouldBlock, "same handle must exclude other threads");
    auto wrong_thread = std::async(std::launch::async, [&] {
        return guard.unlock();
    });
    fails(wrong_thread.get(), ErrorCode::InvalidArgument, "wrong-thread unlock rejected");
    fails(second.try_lock(), ErrorCode::WouldBlock, "wrong-thread failure retained ownership");

    const auto begin = std::chrono::steady_clock::now();
    fails(second.lock_until(begin + external_lock_timeout), ErrorCode::Timeout, "bounded wait");
    check(std::chrono::steady_clock::now() >= begin + external_lock_timeout, "early timeout");

    std::stop_source stop;
    auto canceled = std::async(std::launch::async, [&] {
        return second.lock_until(test_deadline(), stop.get_token());
    });
    check(canceled.wait_for(blocked_wait_observation) == std::future_status::timeout,
          "wait completed while owner still held lock");
    stop.request_stop();
    check(canceled.wait_for(cancellation_timeout) == std::future_status::ready,
          "cancellation did not wake wait");
    fails(canceled.get(), ErrorCode::Cancelled, "cancel result");
    fails(second.try_lock(), ErrorCode::WouldBlock, "timeout/cancel stole lock");

    auto unrelated = take(InterprocessMutex::open(path));
    unrelated.close();
    fails(second.try_lock(), ErrorCode::WouldBlock, "closing another descriptor released lock");
    auto moved = std::move(guard);
    check(!guard && moved, "guard move");
    ok(moved.unlock());
    ok(moved.unlock());
}

void test_guard_lifetime(InterprocessMutex& mutex, InterprocessMutex& second) {
    // Exceptions release via RAII, and closing the public mutex must not
    // release a guard. Native ownership cannot outlive the last guard.
    try {
        auto guard = take(mutex.try_lock());
        throw 1;
    } catch (int) {
    }
    auto guard = take(second.try_lock());
    second.close();
    fails(mutex.try_lock(), ErrorCode::WouldBlock, "close invalidated guard");
    ok(guard.unlock());
    auto acquired = take(mutex.try_lock());
}

void test_local_cancellation(InterprocessMutex& mutex) {
    auto guard = take(mutex.try_lock());
    std::stop_source stop;
    auto waiter = std::async(std::launch::async, [&] {
        return mutex.lock_until(test_deadline(), stop.get_token());
    });
    check(waiter.wait_for(blocked_wait_observation) == std::future_status::timeout,
          "local wait bypassed held mutex");
    stop.request_stop();
    fails(waiter.get(), ErrorCode::Cancelled, "local gate cancellation");
}

void test_file_lock_composition(InterprocessMutex& mutex, const std::filesystem::path& path) {
    // Prove composition: a raw OSAL FileLock excludes the wrapper, and
    // a failed native wait must release the wrapper's local reservation.
    auto raw = take(FileLock::open(path));
    auto guard = take(raw.try_lock());
    fails(mutex.lock_until(std::chrono::steady_clock::now() + file_lock_timeout), ErrorCode::Timeout,
          "wrapper bypassed FileLock");
    ok(guard.unlock());
    auto acquired = take(mutex.try_lock());
    fails(raw.try_lock(), ErrorCode::WouldBlock, "wrapper did not hold underlying FileLock");
}

void test_precancelled_wait(InterprocessMutex& mutex) {
    std::stop_source stop;
    stop.request_stop();
    fails(mutex.lock_until(test_deadline(), stop.get_token()), ErrorCode::Cancelled,
          "pre-canceled wait acquired free lock");
    auto guard = take(mutex.try_lock());
}

void test_local_thread_exclusion(InterprocessMutex& mutex) {
    // Many local threads share a single handle; no recursive flock success.
    std::uint64_t count = 0;
    std::vector<std::future<void>> workers;
    for (int i = 0; i < local_worker_count; ++i) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (int j = 0; j < local_increments_per_worker; ++j) {
                auto guard = take(mutex.lock_until(test_deadline()));
                auto previous_count = count;
                std::this_thread::yield();
                count = previous_count + 1;
            }
        }));
    }
    for (auto& worker : workers) {
        worker.get();
    }
    check(count == local_worker_count * local_increments_per_worker, "local thread exclusion");
}

void test_shared_snapshot(Harness& harness, InterprocessMutex& mutex, const std::filesystem::path& path) {
    const auto name = "mtx" + std::to_string(std::random_device{}());

    struct Cleanup {
        std::string name;

        ~Cleanup() {
            (void)SharedMemory::unlink_name(name);
        }
    } cleanup{name};

    auto memory = take(SharedMemory::create(name, sizeof(Payload)));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    auto* payload = new (take(mapping.writable_bytes()).data()) Payload{};
    std::vector<ProcessWithPipe> children;
    for (int i = 0; i < child_worker_count; ++i) {
        children.push_back(harness.spawn(path, "increment", name));
        receive_token(harness.pipes, children.back().pipe.reader, ready_token);
    }
    for (auto& child_process : children) {
        send_token(harness.pipes, child_process.pipe.writer, start_token);
    }
    for (int i = 0; i < increments_per_process; ++i) {
        increment(mutex, *payload);
    }
    for (auto& child_process : children) {
        receive_token(harness.pipes, child_process.pipe.reader, completed_token);
    }
    {
        auto guard = take(mutex.try_lock());
        check(payload->count == (child_worker_count + 1) * increments_per_process,
              "cross-process increments lost");
    }
    for (auto& child_process : children) {
        send_token(harness.pipes, child_process.pipe.writer, exit_token);
        check(harness.wait(child_process).value == 0, "worker failed");
    }
}

void test_process_exit_cleanup(Harness& harness, InterprocessMutex& mutex,
                               const std::filesystem::path& path) {
    for (const auto* mode : {"abrupt", "kill"}) {
        auto child_process = harness.spawn(path, mode);
        receive_token(harness.pipes, child_process.pipe.reader, ready_token);
        fails(mutex.try_lock(), ErrorCode::WouldBlock, "child did not retain lock");
        if (std::string_view(mode) == "abrupt") {
            send_token(harness.pipes, child_process.pipe.writer, start_token);
        } else {
            (void)take(child_process.process.terminate());
        }
        auto exit = harness.wait(child_process);
        if (std::string_view(mode) == "abrupt") {
            check(exit.value == abrupt_exit_status, "abrupt exit fixture");
        }
        auto guard = take(mutex.lock_until(test_deadline()));
    }
}

int application(const std::vector<std::string>& args) {
    try {
        if (args.size() > 1 && args[1] == "--child") {
            return child(args);
        }

        Workspace files;
        const auto path = files.path / std::filesystem::path(u8"同步 lock.mutex");
        fails(InterprocessMutex::create("relative"), ErrorCode::InvalidArgument, "absolute paths required");
        fails(InterprocessMutex::open(path), ErrorCode::NotFound, "open must not create");
        auto mutex = take(InterprocessMutex::create(path));
        fails(InterprocessMutex::create(path), ErrorCode::AlreadyExists, "exclusive create");
        auto second = take(InterprocessMutex::open(path));
        InterprocessMutex empty;
        fails(empty.try_lock(), ErrorCode::Closed, "closed mutex");

        test_ownership_and_external_wait(mutex, second, path);
        test_guard_lifetime(mutex, second);
        fails(mutex.lock_until(std::chrono::steady_clock::now()), ErrorCode::Timeout, "expired deadline");
        test_local_cancellation(mutex);
        test_file_lock_composition(mutex, path);
        test_precancelled_wait(mutex);
        test_local_thread_exclusion(mutex);

        Harness harness(std::filesystem::absolute(path_from_utf8(args.at(0))));
        test_process_exit_cleanup(harness, mutex, path);
        test_shared_snapshot(harness, mutex, path);

        mutex.close();
        check(std::filesystem::remove(path), "owner removes lock after all handles close");
        auto fresh = take(InterprocessMutex::create(files.path / "new-generation.mutex"));
        auto guard = take(fresh.try_lock());
        std::cout << "PASS: cross-process mutex, thread exclusion, cancellation, guard lifetime, crash and "
                     "kill cleanup\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) {
            return 2;
        }

        std::string text(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr,
                                 nullptr)) {
            return 2;
        }

        text.pop_back();
        args.push_back(std::move(text));
    }
    return application(args);
}

#else
int main(int argc, char** argv) {
    return application(std::vector<std::string>(argv, argv + argc));
}

#endif
