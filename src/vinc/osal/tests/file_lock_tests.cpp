#include <vinc/osal/path.hpp>
// OSAL-only tests: handle-owned file locks, cross-thread guard transfer, pending
// acquisition, independent opens and OS cleanup after actual process death.
#include <vinc/time.hpp>
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/process_pipe.hpp>
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
using namespace std::chrono_literals;

namespace {
constexpr auto operation_timeout = 10s;
constexpr auto lock_timeout_probe = 30ms;
constexpr auto blocked_wait_probe = 20ms;
constexpr auto cancellation_wait_timeout = 2s;
} // namespace

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(vinc::Result<T> result) {
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

template <class T> void fails(const vinc::Result<T>& result, vinc::osal::ErrorCode code, const char* text) {
    check(!result && result.error().is(code), text);
}

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

std::string utf8(const std::filesystem::path& path) {
    auto s = path.u8string();
    // char may inspect char8_t object bytes; the sized copy completes while the UTF-8 owner is alive.
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::filesystem::path path_from(const std::string& text) {
    return take(vinc::osal::path_from_utf8(text));
}

void send(PipeService& pipes, PipeWriter& writer, std::byte token) {
    IoBuffer b(1);
    b.bytes()[0] = token;
    auto id = take(pipes.submit_write(writer, std::move(b), soon()));
    std::array<IoCompletion, 1> done;
    check(take(pipes.wait_completions(done, soon())) == 1 && done[0].id == id &&
              done[0].status == IoStatus::Completed && done[0].transferred == 1,
          "send token");
}

void receive(PipeService& pipes, PipeReader& reader, std::byte token) {
    auto id = take(pipes.submit_read(reader, IoBuffer(1), soon()));
    std::array<IoCompletion, 1> done;
    check(take(pipes.wait_completions(done, soon())) == 1 && done[0].id == id &&
              done[0].status == IoStatus::Completed && done[0].transferred == 1 &&
              done[0].buffer.bytes()[0] == token,
          "receive token");
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
            std::error_code e;
            std::filesystem::remove_all(path, e);
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
        (void)pipes.shutdown_until(soon());
        (void)processes.shutdown_until(soon());
    }

    ProcessWithPipe spawn(const std::filesystem::path& lock, const std::string& mode,
                          const std::string& memory = "") {
        SpawnOptions options;
        options.executable = executable;
        options.arguments = {"--child", utf8(lock), mode, memory};
        auto child = take(spawn_with_pipe(processes, pipes, options));
        children.push_back(child.process.id());
        return child;
    }

    ExitStatus wait(ProcessWithPipe& c) {
        auto result = take(c.process.wait_until(soon()));
        c.pipe.close();
        ok(processes.forget(c.process.id()));
        return result;
    }
};

int child(const std::vector<std::string>& args) {
    auto pipes = take(PipeService::create());
    auto pipe = take(take_inherited_pipe(pipes));
    auto file = take(FileLock::open(path_from(args.at(2))));
    const auto& mode = args.at(3);
    if (mode == "contend") {
        fails(file.try_lock(), vinc::osal::ErrorCode::WouldBlock, "child bypassed parent file lock");
        send(pipes, pipe.writer, std::byte{1});
        receive(pipes, pipe.reader, std::byte{2});
        auto guard = take(file.lock_until(soon()));
        send(pipes, pipe.writer, std::byte{3});
        receive(pipes, pipe.reader, std::byte{4});
        ok(guard.unlock());
        return 0;
    }

    auto guard = take(file.lock_until(soon()));
    file.close();
    send(pipes, pipe.writer, std::byte{1});
    receive(pipes, pipe.reader, std::byte{2});
    if (mode == "abrupt") {
        std::_Exit(17);
    }

    return 99; // kill fixture never receives token 2
}

int application(const std::vector<std::string>& args) {
    try {
        if (args.size() > 1 && args[1] == "--child") {
            return child(args);
        }
        Workspace files;
        const auto path = files.path / std::filesystem::path(u8"檔案 lock.lck");
        fails(FileLock::create("relative"), vinc::osal::ErrorCode::InvalidArgument, "absolute path required");
        fails(FileLock::open(path), vinc::osal::ErrorCode::NotFound, "open created file");
        auto file = take(FileLock::create(path));
        fails(FileLock::create(path), vinc::osal::ErrorCode::AlreadyExists, "exclusive create failed");
        auto peer = take(FileLock::open(path));
        FileLock empty;
        fails(empty.try_lock(), vinc::osal::ErrorCode::Closed, "closed file handle");
        {
            auto guard = take(file.try_lock());
            fails(file.try_lock(), vinc::osal::ErrorCode::Busy, "second guard allowed on same handle");
            // Busy is handle ownership, independent of the caller's thread.
            auto same = std::async(std::launch::async, [&] {
                return file.try_lock();
            });
            fails(same.get(), vinc::osal::ErrorCode::Busy, "file lock imposed mutex thread rules");
            fails(peer.try_lock(), vinc::osal::ErrorCode::WouldBlock, "independent open bypassed lock");
            auto unrelated = take(FileLock::open(path));
            unrelated.close();
            fails(peer.try_lock(), vinc::osal::ErrorCode::WouldBlock, "unrelated close released lock");
            fails(peer.lock_until(std::chrono::steady_clock::now() + lock_timeout_probe),
                  vinc::osal::ErrorCode::Timeout, "deadline ignored");
            std::stop_source stop;
            auto waiter = std::async(std::launch::async, [&] {
                return peer.lock_until(soon(), stop.get_token());
            });
            check(waiter.wait_for(blocked_wait_probe) == std::future_status::timeout, "wait did not block");
            fails(peer.try_lock(), vinc::osal::ErrorCode::Busy, "pending acquisition did not reserve handle");
            stop.request_stop();
            check(waiter.wait_for(cancellation_wait_timeout) == std::future_status::ready,
                  "cancel did not wake");
            fails(waiter.get(), vinc::osal::ErrorCode::Cancelled, "cancellation result");
            fails(peer.try_lock(), vinc::osal::ErrorCode::WouldBlock,
                  "cancel stole lock or leaked reservation");
            // A FileLockGuard is intentionally transferable across threads.
            auto handoff = std::async(std::launch::async, [g = std::move(guard)]() mutable {
                return g.unlock();
            });
            ok(handoff.get());
            check(!guard, "guard move retained ownership");
            auto acquired = take(peer.try_lock());
            ok(acquired.unlock());
            ok(acquired.unlock());
        }
        {
            auto guard = take(peer.try_lock());
            peer.close();
            fails(file.try_lock(), vinc::osal::ErrorCode::WouldBlock, "guard failed to retain closed handle");
            auto handoff = std::async(std::launch::async, [g = std::move(guard)]() mutable {
                auto local = std::move(g);
            });
            handoff.get(); // The local guard's destructor runs on the receiving thread.
            try {
                auto acquired = take(file.try_lock());
                throw 1;
            } catch (int) {
            }

            auto acquired = take(file.try_lock());
        }
        fails(file.lock_until(std::chrono::steady_clock::now()), vinc::osal::ErrorCode::Timeout,
              "expired deadline acquired");
        std::stop_source canceled;
        canceled.request_stop();
        fails(file.lock_until(soon(), canceled.get_token()), vinc::osal::ErrorCode::Cancelled,
              "pre-canceled acquired");
        Harness h(std::filesystem::absolute(path_from(args.at(0))));
        {
            auto guard = take(file.try_lock());
            auto c = h.spawn(path, "contend");
            receive(h.pipes, c.pipe.reader, std::byte{1});
            ok(guard.unlock());
            send(h.pipes, c.pipe.writer, std::byte{2});
            receive(h.pipes, c.pipe.reader, std::byte{3});
            fails(file.try_lock(), vinc::osal::ErrorCode::WouldBlock, "child guard failed to exclude parent");
            send(h.pipes, c.pipe.writer, std::byte{4});
            check(h.wait(c).value == 0, "contender failed");
        }

        for (const auto* mode : {"abrupt", "kill"}) {
            auto c = h.spawn(path, mode);
            receive(h.pipes, c.pipe.reader, std::byte{1});
            fails(file.try_lock(), vinc::osal::ErrorCode::WouldBlock, "child guard failed");
            if (std::string_view(mode) == "abrupt") {
                send(h.pipes, c.pipe.writer, std::byte{2});
            } else {
                (void)take(c.process.terminate());
            }

            const auto status = h.wait(c);
            if (std::string_view(mode) == "abrupt") {
                check(status.value == 17, "abrupt fixture failed");
            }

            auto recovered = take(file.lock_until(soon()));
        }
        file.close();
        check(std::filesystem::remove(path), "quiescent resource cleanup");
        std::cout
            << "PASS: FileLock handle ownership, thread handoff, deadlines, cancellation and process death\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "file_lock_test_main.hpp"
