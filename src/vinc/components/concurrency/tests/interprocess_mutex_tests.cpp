// Controlled IPC handshakes test failure ordering, never sleep-based crash guesses.
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/concurrency/detail/mutex_recovery.hpp>
#include <vinc/concurrency/detail/interprocess_mutex_state.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <array>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace vinc::osal;
using vinc::concurrency::InterprocessMutex;
using vinc::concurrency::detail::MutexRecovery;
using Code = vinc::concurrency::ErrorCode;
using namespace std::chrono_literals;

namespace {
constexpr auto operation_timeout = 10s;
constexpr auto blocked_timeout = 30ms;
constexpr auto cancellation_timeout = 2s;
constexpr int worker_count = 3;
constexpr int increments_per_worker = 100;
constexpr std::size_t snapshot_values = 64;
constexpr int crash_code = 17;
constexpr std::byte ready_token{1};
constexpr std::byte start_token{2};
constexpr vinc::OwnerId first_child_owner = 1;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
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

template<class T> void fails(const vinc::Result<T>& result, Code code, const char* reason) {
    if (result) {
        throw std::runtime_error(std::string(reason) + ": unexpectedly succeeded");
    }

    if (!result.error().is(code)) {
        throw std::runtime_error(std::string(reason) + ": " + vinc::error_to_string(result.error()));
    }
}

vinc::Deadline deadline() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

std::string unique_name() {
    return "mx" + std::to_string(std::random_device{}());
}

struct Names {
    std::vector<std::string> names;
    std::string next() {
        auto name = unique_name();
        names.push_back(name);
        return name;
    }
    ~Names() {
        for (const auto& name : names) {
            (void)SharedMemory::unlink_name(name);
        }
    }
};

void send_token(PipeService& pipes, PipeWriter& writer, std::byte token) {
    IoBuffer buffer(1);
    buffer.bytes()[0] = token;
    auto id = take(pipes.submit_write(writer, std::move(buffer), deadline()));
    std::array<IoCompletion, 1> completed;
    check(take(pipes.wait_completions(completed, deadline())) == 1 &&
              completed[0].id == id && completed[0].status == IoStatus::Completed &&
              completed[0].transferred == 1, "fixture token write");
}

void receive_token(PipeService& pipes, PipeReader& reader, std::byte token) {
    auto id = take(pipes.submit_read(reader, IoBuffer(1), deadline()));
    std::array<IoCompletion, 1> completed;
    check(take(pipes.wait_completions(completed, deadline())) == 1 &&
              completed[0].id == id && completed[0].status == IoStatus::Completed &&
              completed[0].transferred == 1 && completed[0].buffer.bytes()[0] == token,
          "fixture token read");
}

struct Snapshot {
    std::uint64_t revision = 0;
    std::array<std::uint64_t, snapshot_values> values{};
};
struct Payload {
    std::array<Snapshot, 2> slots{};
};

template<class T> T* mapped_object(SharedMemoryMapping& mapping) {
    // Test-only shared ABI boundary: the OS mapping has page alignment and
    // sizeof(T) live storage; parent constructs T before handing its name to peers.
    return reinterpret_cast<T*>(take(mapping.writable_bytes()).data());
}

void validate(const Snapshot& snapshot) {
    for (auto value : snapshot.values) {
        check(value == snapshot.revision, "torn published snapshot");
    }
}

void increment(InterprocessMutex& mutex, Payload& payload) {
    auto guard = take(mutex.lock_until(deadline()));
    auto& current = payload.slots[guard.publication()];
    validate(current);
    auto& candidate = payload.slots[!guard.publication()];
    candidate.revision = current.revision + 1;
    for (auto& value : candidate.values) {
        value = candidate.revision;
        std::this_thread::yield();
    }
    ok(guard.publish_and_unlock());
}

int child(const std::vector<std::string>& args) {
    auto pipes = take(PipeService::create());
    auto connection = take(take_inherited_pipe(pipes));
    const auto mode = args.at(3);
    const auto identity = std::stoull(args.at(5));
    auto mutex = take(InterprocessMutex::open(args.at(2), identity));
    auto memory = take(SharedMemory::open(args.at(4), SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    auto* payload = mapped_object<Payload>(mapping);

    if (mode == "increments") {
        send_token(pipes, connection.writer, ready_token);
        receive_token(pipes, connection.reader, start_token);
        for (int count = 0; count < increments_per_worker; ++count) {
            increment(mutex, *payload);
        }
        connection.close();
        ok(pipes.shutdown_until(deadline()));
        return 0;
    }

    auto guard = take(mutex.lock_until(deadline()));
    auto& candidate = payload->slots[!guard.publication()];
    candidate.revision = 1;
    const auto writes = mode == "partial" || mode == "kill" ? snapshot_values / 2 : snapshot_values;
    for (std::size_t index = 0; index < writes; ++index) {
        candidate.values[index] = 1;
    }
    mutex.close(); // Guard alone must retain the mapping during the crash.
    send_token(pipes, connection.writer, ready_token);
    receive_token(pipes, connection.reader, start_token);
    if (mode == "commit") {
        ok(guard.publish_and_unlock());
    }
    if (mode == "wrong-thread") {
        std::thread other([held = std::move(guard)] {});
        other.join();
        return 99;
    }
    std::_Exit(crash_code);
}

struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes = take(PipeService::create());
    std::filesystem::path executable;
    std::vector<ProcessId> children;

    explicit Harness(std::filesystem::path path) : executable(std::move(path)) {}
    ~Harness() {
        for (auto id : children) {
            (void)processes.terminate(id);
        }
        (void)pipes.shutdown_until(deadline());
        (void)processes.shutdown_until(deadline());
    }

    ProcessWithPipe spawn(const std::string& lock, const std::string& mode,
                          const std::string& payload, vinc::OwnerId owner) {
        SpawnOptions options;
        options.executable = executable;
        options.arguments = {"--child", lock, mode, payload, std::to_string(owner)};
        auto spawned = take(spawn_with_pipe(processes, pipes, options));
        children.push_back(spawned.process.id());
        return spawned;
    }

    ExitStatus wait(ProcessWithPipe& child_process) {
        auto status = take(child_process.process.wait_until(deadline()));
        child_process.pipe.close();
        ok(processes.forget(child_process.process.id()));
        return status;
    }
};

void test_validation_and_lifetime(Names& names) {
    fails(InterprocessMutex::create(names.next(), vinc::no_owner), Code::InvalidArgument, "zero owner");
    fails(InterprocessMutex::create(names.next(), vinc::central_owner + 1), Code::InvalidArgument,
          "owner truncation forbidden");
    fails(InterprocessMutex::create("bad/name", vinc::central_owner), Code::InvalidArgument,
          "native invalid name");
    const auto missing = InterprocessMutex::open(names.next(), vinc::central_owner);
    fails(missing, Code::NotFound, "open cannot create");
    check(missing.error().causes().size() == 1, "OSAL cause missing");

    const auto name = names.next();
    auto mutex = take(InterprocessMutex::create(name, vinc::central_owner));
    fails(InterprocessMutex::create(name, vinc::central_owner), Code::AlreadyExists, "exclusive create");
    // Both handles are in the central process and must use its one identity.
    auto peer = take(InterprocessMutex::open(name, vinc::central_owner));
    auto guard = take(mutex.try_lock());
    check(!guard.publication() && !guard.recovered(), "initial guard state");
    fails(mutex.try_lock(), Code::Busy, "same-handle recursion");
    fails(peer.try_lock(), Code::WouldBlock, "independent handle exclusion");
    check(take(peer.owner()) == vinc::central_owner, "owner snapshot");
    auto wrong_thread = std::async(std::launch::async, [&] { return guard.unlock(); });
    fails(wrong_thread.get(), Code::InvalidArgument, "wrong-thread explicit unlock");
    mutex.close();
    ok(guard.publish_and_unlock());
    ok(guard.unlock());
    {
        auto held = take(peer.try_lock());
        check(held.publication(), "publication survived owner close");
    }
    {
        auto held = take(peer.try_lock());
        check(held.publication(), "RAII must not flip publication");
    }
    auto movable = take(peer.try_lock());
    auto moved = std::move(movable);
    check(!movable && bool(moved), "guard move ownership");
    ok(moved.unlock());
    fails(mutex.try_lock(), Code::Closed, "closed object");
}

void test_bad_layout(Names& names) {
    using Header = vinc::concurrency::detail::MutexHeader;
    const auto name = names.next();
    auto memory = take(SharedMemory::create(name, sizeof(Header)));
    auto mapping = take(memory.map(sizeof(Header), SharedMemoryAccess::ReadWrite));
    // No concurrent peer: intentionally malformed headers exercise attach validation.
    fails(InterprocessMutex::open(name, vinc::central_owner), Code::InvalidLayout, "uninitialized region");
    auto* header = std::construct_at(mapped_object<Header>(mapping));
    header->version += 1;
    fails(InterprocessMutex::open(name, vinc::central_owner), Code::InvalidLayout, "unknown version");
    header->version = vinc::concurrency::detail::mutex_layout_version;
    header->bytes = 0;
    fails(InterprocessMutex::open(name, vinc::central_owner), Code::InvalidLayout, "wrong length");
}

void test_waiting(Names& names) {
    const auto name = names.next();
    auto mutex = take(InterprocessMutex::create(name, vinc::central_owner));
    auto peer = take(InterprocessMutex::open(name, vinc::central_owner));
    std::stop_source cancelled;
    cancelled.request_stop();
    fails(peer.lock_until(deadline(), cancelled.get_token()), Code::Cancelled, "precancelled free lock");
    fails(peer.lock_until(std::chrono::steady_clock::now()), Code::Timeout, "expired free lock");

    auto held = take(mutex.try_lock());
    fails(peer.lock_until(std::chrono::steady_clock::now() + blocked_timeout), Code::Timeout,
          "contended timeout");
    std::stop_source stop;
    std::promise<void> entered;
    auto started = entered.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        return peer.lock_until(deadline(), stop.get_token());
    });
    started.wait();
    stop.request_stop();
    check(waiter.wait_for(cancellation_timeout) == std::future_status::ready, "cancellation while held");
    fails(waiter.get(), Code::Cancelled, "cancel result");
    // A different thread on the SAME handle sees contention, never false recursion.
    auto other = std::async(std::launch::async, [&] { return mutex.try_lock(); });
    fails(other.get(), Code::WouldBlock, "local contention");
    ok(held.unlock());
    auto next = take(peer.try_lock());
    ok(next.unlock());
}

void test_local_threads(Names& names) {
    auto mutex = take(InterprocessMutex::create(names.next(), vinc::central_owner));
    Payload payload;
    std::vector<std::future<void>> workers;
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (int count = 0; count < increments_per_worker; ++count) {
                increment(mutex, payload);
            }
        }));
    }
    for (auto& worker : workers) {
        worker.get();
    }
    auto held = take(mutex.try_lock());
    const auto& active = payload.slots[held.publication()];
    validate(active);
    check(active.revision == worker_count * increments_per_worker, "local lost update");
}

void test_processes(Harness& harness, Names& names) {
    const auto lock_name = names.next();
    const auto payload_name = names.next();
    auto mutex = take(InterprocessMutex::create(lock_name, vinc::central_owner));
    auto memory = take(SharedMemory::create(payload_name, sizeof(Payload)));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    auto* payload = std::construct_at(mapped_object<Payload>(mapping));
    std::vector<ProcessWithPipe> children;
    for (int worker = 0; worker < worker_count; ++worker) {
        children.push_back(harness.spawn(lock_name, "increments", payload_name, first_child_owner + worker));
        receive_token(harness.pipes, children.back().pipe.reader, ready_token);
    }
    for (auto& spawned : children) {
        send_token(harness.pipes, spawned.pipe.writer, start_token);
    }
    for (auto& spawned : children) {
        check(harness.wait(spawned).value == 0, "child increments failed");
    }
    auto held = take(mutex.try_lock());
    const auto& active = payload->slots[held.publication()];
    validate(active);
    check(active.revision == worker_count * increments_per_worker, "cross-process lost update");
}

void test_crash(Harness& harness, Names& names, const std::string& mode) {
    const auto lock_name = names.next();
    const auto payload_name = names.next();
    auto mutex = take(InterprocessMutex::create(lock_name, vinc::central_owner));
    auto memory = take(SharedMemory::create(payload_name, sizeof(Payload)));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    auto* payload = std::construct_at(mapped_object<Payload>(mapping));
    auto spawned = harness.spawn(lock_name, mode, payload_name, first_child_owner);
    receive_token(harness.pipes, spawned.pipe.reader, ready_token);
    fails(mutex.try_lock(), Code::WouldBlock, "child should hold lock");
    check(take(mutex.owner()) == first_child_owner, "child identity missing");
    check(!take(spawned.process.try_wait()).has_value(), "child must be alive at handshake");

    if (mode == "kill") {
        take(spawned.process.terminate());
    } else {
        send_token(harness.pipes, spawned.pipe.writer, start_token);
    }
    const auto status = harness.wait(spawned); // Proof of exit BEFORE recovery.
    if (mode != "kill" && mode != "wrong-thread") {
        check(status.value == crash_code, "expected abrupt exit");
    } else if (mode == "wrong-thread") {
        check(status.value != 0 && status.value != 99, "wrong-thread destruction must be fatal");
    }

    if (mode == "commit") {
        auto held = take(mutex.try_lock());
        check(held.publication(), "committed selection lost");
        validate(payload->slots[held.publication()]);
        check(payload->slots[held.publication()].revision == 1, "committed value lost");
        ok(held.unlock());
        fails(MutexRecovery::acquire_after_exit(mutex, first_child_owner), Code::WouldBlock,
              "stale recovery after commit");
        return;
    }

    fails(mutex.try_lock(), Code::WouldBlock, "OS must not silently clear a shared flag");
    fails(MutexRecovery::acquire_after_exit(mutex, first_child_owner + 1), Code::WouldBlock,
          "wrong dead identity must not clear owner");
    auto held = take(MutexRecovery::acquire_after_exit(mutex, first_child_owner));
    check(held.recovered() && !held.publication(), "recovery must preserve previous selection");
    validate(payload->slots[held.publication()]);
    check(payload->slots[held.publication()].revision == 0, "uncommitted candidate published");
    ok(held.unlock());

    auto next = take(mutex.try_lock());
    auto supervisor_view = take(InterprocessMutex::open(lock_name, vinc::central_owner));
    fails(MutexRecovery::acquire_after_exit(supervisor_view, first_child_owner), Code::WouldBlock,
          "stale recovery cannot steal a new live guard");
    ok(next.unlock());
    // Reuse the candidate fully; abandoned partial writes must never leak through.
    increment(mutex, *payload);
    auto final = take(mutex.try_lock());
    validate(payload->slots[final.publication()]);
}

void test_recovery_permissions_and_epochs(Names& names) {
    auto old = take(InterprocessMutex::create(names.next(), vinc::central_owner));
    auto held = take(old.try_lock());
    // New epoch can reuse the numeric identity, never the old resource name.
    auto fresh = take(InterprocessMutex::create(names.next(), vinc::central_owner));
    auto new_guard = take(fresh.try_lock());
    fails(MutexRecovery::acquire_after_exit(old, vinc::central_owner), Code::InvalidArgument,
          "central crash retires epoch, never recovers old central");
    fails(MutexRecovery::acquire_after_exit(old, vinc::no_owner), Code::InvalidArgument,
          "empty is not a dead identity");
    auto fixture = take(InterprocessMutex::create(names.next(), first_child_owner));
    fails(MutexRecovery::acquire_after_exit(fixture, first_child_owner + 1), Code::InvalidArgument,
          "noncentral binding cannot reclaim");
    auto maximum = take(InterprocessMutex::create(names.next(), vinc::max_module_owner));
    auto maximum_guard = take(maximum.try_lock());
    check(take(maximum.owner()) == vinc::max_module_owner, "maximum owner was truncated");
    ok(maximum_guard.publish_and_unlock());
    auto again = take(maximum.try_lock());
    check(again.publication(), "maximum owner overlaps publication bit");
}

int application(const std::vector<std::string>& args) {
    try {
        if (args.size() > 1 && args[1] == "--child") {
            return child(args);
        }

        Names names;
        test_validation_and_lifetime(names);
        test_bad_layout(names);
        test_waiting(names);
        test_local_threads(names);
        test_recovery_permissions_and_epochs(names);
        Harness harness(std::filesystem::absolute(take(path_from_utf8(args.at(0)))));
        test_processes(harness, names);
        for (const auto* mode : {"partial", "complete", "commit", "kill", "wrong-thread"}) {
            test_crash(harness, names, mode);
        }
        std::cout << "PASS: shared mutex, atomic publication, cancellation, process recovery and epochs\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int index = 0; index < argc; ++index) {
        auto text = path_to_utf8(std::filesystem::path(argv[index]));
        if (!text) {
            return 2;
        }
        args.push_back(std::move(*text));
    }
    return application(args);
}
#else
int main(int argc, char** argv) {
    return application(std::vector<std::string>(argv, argv + argc));
}
#endif
