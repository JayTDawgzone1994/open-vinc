// Real processes contend over mapped payloads and die while holding the lock.
// Pipe handshakes distinguish a held lock from a child that has not started yet.
#include <vinc/sync/interprocess_mutex.hpp>
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
using vinc::sync::InterprocessMutex;
using namespace std::chrono_literals;
std::string describe(const OsError& e) { return e.native.message(); }
std::string describe(const vinc::sync::Error& e) { return e.os_error ? e.os_error->native.message() : std::string{}; }
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T, class E> T take(std::expected<T, E> result) {
    if (!result) throw std::runtime_error("OSAL code=" + std::to_string(static_cast<int>(result.error().code)) + " " + describe(result.error()));
    return std::move(*result);
}
template<class E> void ok(std::expected<void, E> result) { if (!result) throw std::runtime_error(describe(result.error())); }
template<class T, class E> void fails(const std::expected<T, E>& result, ErrorCode code, const char* text) { check(!result && result.error().code == code, text); }
Deadline soon() { return std::chrono::steady_clock::now() + 10s; }
std::string utf8(const std::filesystem::path& path) { auto s = path.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
std::filesystem::path path_from(const std::string& text) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())); }
void send(PipeService& pipes, PipeWriter& writer, std::byte token) {
    IoBuffer b(1); b.bytes()[0] = token; auto id = take(pipes.submit_write(writer, std::move(b), soon()));
    std::array<IoCompletion, 1> done;
    check(take(pipes.wait_completions(done, soon())) == 1 && done[0].id == id && done[0].status == IoStatus::Completed && done[0].transferred == 1, "send token");
}
void receive(PipeService& pipes, PipeReader& reader, std::byte token) {
    auto id = take(pipes.submit_read(reader, IoBuffer(1), soon())); std::array<IoCompletion, 1> done;
    check(take(pipes.wait_completions(done, soon())) == 1 && done[0].id == id && done[0].status == IoStatus::Completed &&
        done[0].transferred == 1 && done[0].buffer.bytes()[0] == token, "receive token");
}
struct Payload { SharedWord active{}; std::uint64_t count = 0; std::array<std::uint64_t, 64> values{}; };
void increment(InterprocessMutex& mutex, Payload& payload) {
    auto guard = take(mutex.lock_until(soon()));
    check(shared_fetch_add(payload.active, 1) == 0, "overlapping critical sections");
    for (auto v : payload.values) check(v == payload.count, "torn shared snapshot");
    const auto next = payload.count + 1;
    for (auto& v : payload.values) { v = next; std::this_thread::yield(); }
    payload.count = next;
    check(shared_fetch_add(payload.active, std::uint64_t(-1)) == 1, "active owner mismatch");
    ok(guard.unlock());
}
int child(const std::vector<std::string>& args) {
    auto pipes = take(PipeService::create()); auto pipe = take(take_inherited_pipe(pipes));
    auto mutex = take(InterprocessMutex::open(path_from(args.at(2)))); const auto& mode = args.at(3);
    if (mode == "abrupt" || mode == "kill") {
        auto guard = take(mutex.lock_until(soon()));
        mutex.close(); // The guard alone must retain the native handle.
        send(pipes, pipe.writer, std::byte{1});
        receive(pipes, pipe.reader, std::byte{2});
        if (mode == "abrupt") std::_Exit(17); // Skip all C++ cleanup.
        return 99; // kill test never sends this token
    }
    auto memory = take(SharedMemory::open(args.at(4), SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
    auto* payload = reinterpret_cast<Payload*>(take(mapping.writable_bytes()).data());
    send(pipes, pipe.writer, std::byte{1}); receive(pipes, pipe.reader, std::byte{2});
    for (int i = 0; i < 200; ++i) increment(mutex, *payload);
    send(pipes, pipe.writer, std::byte{3}); receive(pipes, pipe.reader, std::byte{4});
    pipe.close(); ok(pipes.shutdown_until(soon())); return 0;
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
    ~Workspace() { if (path.parent_path() == root) { std::error_code e; std::filesystem::remove_all(path, e); } }
};
struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes = take(PipeService::create());
    std::filesystem::path executable;
    std::vector<ProcessId> children;
    explicit Harness(std::filesystem::path exe) : executable(std::move(exe)) {}
    ~Harness() {
        for (auto id : children) (void)processes.terminate(id);
        (void)pipes.shutdown_until(soon()); (void)processes.shutdown_until(soon());
    }
    ProcessWithPipe spawn(const std::filesystem::path& lock, const std::string& mode, const std::string& memory = "") {
        SpawnOptions options; options.executable = executable; options.arguments = {"--child", utf8(lock), mode, memory};
        auto child = take(spawn_with_pipe(processes, pipes, options)); children.push_back(child.process.id()); return child;
    }
    ExitStatus wait(ProcessWithPipe& c) { auto result = take(c.process.wait_until(soon())); c.pipe.close(); ok(processes.forget(c.process.id())); return result; }
};
int application(const std::vector<std::string>& args) {
    try {
        if (args.size() > 1 && args[1] == "--child") return child(args);
        Workspace files;
        const auto path = files.path / std::filesystem::path(u8"同步 lock.mutex");
        fails(InterprocessMutex::create("relative"), ErrorCode::InvalidArgument, "absolute paths required");
        fails(InterprocessMutex::open(path), ErrorCode::NotFound, "open must not create");
        auto mutex = take(InterprocessMutex::create(path));
        fails(InterprocessMutex::create(path), ErrorCode::AlreadyExists, "exclusive create");
        auto second = take(InterprocessMutex::open(path));
        InterprocessMutex empty;
        fails(empty.try_lock(), ErrorCode::Closed, "closed mutex");
        {
            auto guard = take(mutex.try_lock());
            fails(mutex.try_lock(), ErrorCode::Busy, "nonrecursive same handle");
            fails(second.try_lock(), ErrorCode::WouldBlock, "separate handle must contend");
            auto another = std::async(std::launch::async, [&] { return mutex.try_lock(); });
            fails(another.get(), ErrorCode::WouldBlock, "same handle must exclude other threads");
            auto wrong_thread = std::async(std::launch::async, [&] { return guard.unlock(); });
            fails(wrong_thread.get(), ErrorCode::InvalidArgument, "wrong-thread unlock rejected");
            fails(second.try_lock(), ErrorCode::WouldBlock, "wrong-thread failure retained ownership");
            const auto begin = std::chrono::steady_clock::now();
            fails(second.lock_until(begin + 60ms), ErrorCode::Timeout, "bounded wait");
            check(std::chrono::steady_clock::now() >= begin + 60ms, "early timeout");
            std::stop_source stop;
            auto canceled = std::async(std::launch::async, [&] { return second.lock_until(soon(), stop.get_token()); });
            check(canceled.wait_for(20ms) == std::future_status::timeout, "wait completed while owner still held lock");
            stop.request_stop();
            check(canceled.wait_for(2s) == std::future_status::ready, "cancellation did not wake wait");
            fails(canceled.get(), ErrorCode::Cancelled, "cancel result");
            fails(second.try_lock(), ErrorCode::WouldBlock, "timeout/cancel stole lock");
            auto unrelated = take(InterprocessMutex::open(path)); unrelated.close();
            fails(second.try_lock(), ErrorCode::WouldBlock, "closing another descriptor released lock");
            auto moved = std::move(guard); check(!guard && moved, "guard move");
            ok(moved.unlock()); ok(moved.unlock());
        }
        {
            // Exceptions release via RAII, and closing the public mutex must not
            // release a guard. Native ownership cannot outlive the last guard.
            try { auto guard = take(mutex.try_lock()); throw 1; } catch (int) {}
            auto guard = take(second.try_lock()); second.close();
            fails(mutex.try_lock(), ErrorCode::WouldBlock, "close invalidated guard");
            ok(guard.unlock());
            auto acquired = take(mutex.try_lock());
        }
        fails(mutex.lock_until(std::chrono::steady_clock::now()), ErrorCode::Timeout, "expired deadline");
        {
            auto guard = take(mutex.try_lock());
            std::stop_source stop;
            auto waiter = std::async(std::launch::async, [&] { return mutex.lock_until(soon(), stop.get_token()); });
            check(waiter.wait_for(20ms) == std::future_status::timeout, "local wait bypassed held mutex");
            stop.request_stop(); fails(waiter.get(), ErrorCode::Cancelled, "local gate cancellation");
        }
        {
            // Prove composition: a raw OSAL FileLock excludes the wrapper, and
            // a failed native wait must release the wrapper's local reservation.
            auto raw = take(FileLock::open(path)); auto guard = take(raw.try_lock());
            fails(mutex.lock_until(std::chrono::steady_clock::now() + 30ms), ErrorCode::Timeout, "wrapper bypassed FileLock");
            ok(guard.unlock()); auto acquired = take(mutex.try_lock());
            fails(raw.try_lock(), ErrorCode::WouldBlock, "wrapper did not hold underlying FileLock");
        }
        {
            std::stop_source stop; stop.request_stop();
            fails(mutex.lock_until(soon(), stop.get_token()), ErrorCode::Cancelled, "pre-canceled wait acquired free lock");
            auto guard = take(mutex.try_lock());
        }
        {
            // Many local threads share a single handle; no recursive flock success.
            std::uint64_t count = 0; std::vector<std::future<void>> workers;
            for (int i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, [&] {
                for (int j = 0; j < 100; ++j) { auto guard = take(mutex.lock_until(soon())); auto n = count; std::this_thread::yield(); count = n + 1; }
            }));
            for (auto& worker : workers) worker.get();
            check(count == 400, "local thread exclusion");
        }
        Harness h(std::filesystem::absolute(path_from(args.at(0))));
        for (const auto* mode : {"abrupt", "kill"}) {
            auto c = h.spawn(path, mode); receive(h.pipes, c.pipe.reader, std::byte{1});
            fails(mutex.try_lock(), ErrorCode::WouldBlock, "child did not retain lock");
            if (std::string_view(mode) == "abrupt") send(h.pipes, c.pipe.writer, std::byte{2});
            else (void)take(c.process.terminate());
            auto exit = h.wait(c);
            if (std::string_view(mode) == "abrupt") check(exit.value == 17, "abrupt exit fixture");
            auto guard = take(mutex.lock_until(soon()));
        }
        {
            const auto name = "mtx" + std::to_string(std::random_device{}());
            struct Cleanup { std::string name; ~Cleanup() { (void)SharedMemory::unlink_name(name); } } cleanup{name};
            auto memory = take(SharedMemory::create(name, sizeof(Payload)));
            auto mapping = take(memory.map(sizeof(Payload), SharedMemoryAccess::ReadWrite));
            auto* payload = new (take(mapping.writable_bytes()).data()) Payload{};
            std::vector<ProcessWithPipe> children;
            for (int i = 0; i < 3; ++i) { children.push_back(h.spawn(path, "increment", name)); receive(h.pipes, children.back().pipe.reader, std::byte{1}); }
            for (auto& c : children) send(h.pipes, c.pipe.writer, std::byte{2});
            for (int i = 0; i < 200; ++i) increment(mutex, *payload);
            for (auto& c : children) receive(h.pipes, c.pipe.reader, std::byte{3});
            { auto guard = take(mutex.try_lock()); check(payload->count == 800, "cross-process increments lost"); }
            for (auto& c : children) { send(h.pipes, c.pipe.writer, std::byte{4}); check(h.wait(c).value == 0, "worker failed"); }
        }
        mutex.close();
        check(std::filesystem::remove(path), "owner removes lock after all handles close");
        auto fresh = take(InterprocessMutex::create(files.path / "new-generation.mutex"));
        auto guard = take(fresh.try_lock());
        std::cout << "PASS: cross-process mutex, thread exclusion, cancellation, guard lifetime, crash and kill cleanup\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) return 2;
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr, nullptr)) return 2;
        text.pop_back(); args.push_back(std::move(text));
    }
    return application(args);
}
#else
int main(int argc, char** argv) { return application(std::vector<std::string>(argv, argv + argc)); }
#endif
