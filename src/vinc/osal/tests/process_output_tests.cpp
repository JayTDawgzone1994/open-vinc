// Real child stdio, independent of framework modules, logging and shell policy.
// Every read/wait is bounded; the harness terminates children on test failure.
#include <vinc/osal/process_pipe.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace vinc::osal;
using namespace std::chrono_literals;
static_assert(std::is_nothrow_move_constructible_v<ProcessWithOutput>);
static_assert(!std::is_copy_constructible_v<ProcessWithOutput>);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error("OSAL " + std::to_string(static_cast<int>(result.error().code)) + ": " +
                                 result.error().native.message());
    }

    return std::move(*result);
}

Deadline soon() {
    return std::chrono::steady_clock::now() + 10s;
}

struct Harness {
    ProcessService processes;
    PipeService pipes;
    std::filesystem::path helper;
    std::vector<ProcessId> children;

    Harness(const std::filesystem::path& path, std::size_t endpoints = 8, std::size_t records = 32)
        : processes(take(ProcessService::create(ServiceOptions{records}))),
          pipes(take(PipeService::create(PipeServiceOptions{endpoints}))), helper(path) {
        children.reserve(records);
    }

    ~Harness() {
        for (auto id : children) {
            auto ignored = processes.terminate(id);
            (void)ignored;
        }

        auto io = pipes.shutdown_until(soon());
        (void)io;
        auto exited = processes.shutdown_until(soon());
        (void)exited;
    }

    SpawnOptions options(const std::string& mode) const {
        SpawnOptions result;
        result.executable = helper;
        result.arguments = {mode};
        result.stdout_mode = result.stderr_mode = StdioMode::Capture;
        return result;
    }

    ProcessWithOutput spawn(const SpawnOptions& options, std::span<const PipeBinding> bindings = {}) {
        auto result = take(spawn_with_output(processes, pipes, options, bindings));
        children.push_back(result.process.id());
        return result;
    }

    IoCompletion next() {
        std::array<IoCompletion, 1> output;
        require(take(pipes.wait_completions(output, soon())) == 1, "missing completion");
        return std::move(output[0]);
    }

    // Read all streams at once: sequentially draining stdout then stderr can
    // deadlock a child that fills both pipes. Tiny chunks exercise byte framing.
    std::vector<std::string> drain(std::span<PipeReader*> readers) {
        const auto deadline = soon();
        std::vector<std::string> bytes(readers.size());
        std::vector<bool> ended(readers.size(), false);
        std::size_t remaining = readers.size();
        for (auto* reader : readers) {
            take(pipes.submit_read(*reader, IoBuffer(997), deadline));
        }
        while (remaining) {
            auto item = next();
            std::size_t i = 0;
            while (i < readers.size() && readers[i]->id() != item.pipe) {
                ++i;
            }
            require(i < readers.size() && !ended[i], "unexpected completion identity");
            if (item.status == IoStatus::EndOfStream) {
                ended[i] = true;
                --remaining;
                continue;
            }
            require(item.status == IoStatus::Completed && item.transferred > 0, "capture read failed");
            require(bytes[i].size() + item.transferred <= 2 * 1024 * 1024, "unexpected unbounded output");
            bytes[i].append(reinterpret_cast<const char*>(item.buffer.bytes().data()), item.transferred);
            take(pipes.submit_read(*readers[i], IoBuffer(997), deadline));
        }

        return bytes;
    }

    std::vector<std::string> drain(ProcessWithOutput& child) {
        std::vector<PipeReader*> readers;
        if (child.stdout_reader) {
            readers.push_back(&*child.stdout_reader);
        }

        if (child.stderr_reader) {
            readers.push_back(&*child.stderr_reader);
        }

        return drain(readers);
    }
};

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected plain helper and pipe helper paths");
        const auto helper = std::filesystem::absolute(argv[1]);
        const auto pipe_helper = std::filesystem::absolute(argv[2]);
        const std::string expected_out = std::string("printf:") + std::string("A\0\xE4\xB8\xAD", 5);
        const std::string expected_err = "fprintf:cerr";
        {
            Harness test(helper);
            auto child = test.spawn(test.options("stdio-small"));
            require(child.stdout_reader && child.stderr_reader, "capture readers missing");
            require(child.stdout_reader->id() != child.stderr_reader->id(), "streams share identity");
            require(take(child.process.wait_until(soon())).value == 0, "plain stdio helper failed");
            // Wait first only for this bounded, small output. Buffered bytes must
            // survive child exit and moving the result before the first read.
            auto moved = std::move(child);
            const auto bytes = test.drain(moved);
            require(bytes[0] == expected_out && bytes[1] == expected_err, "stdio tail/bytes corrupted");
        }

        std::cout << "PASS: unlinked executable, printf/cout, independent streams, binary tail after exit\n";
        {
            Harness test(helper);
            auto child = test.spawn(test.options("stdio-large"));
            const auto bytes = test.drain(child);
            for (std::size_t stream = 0; stream < bytes.size(); ++stream) {
                require(bytes[stream].size() == 1024 * 1024 + 17, "large output truncated");
                const auto seed = stream == 0 ? 11 : 73;
                for (std::size_t i = 0; i < bytes[stream].size(); ++i) {
                    require(static_cast<unsigned char>(bytes[stream][i]) == ((i * 37 + seed) & 255),
                            "large output corrupt");
                }
            }
            require(take(child.process.wait_until(soon())).value == 0, "large writer failed");
        }

        std::cout << "PASS: concurrent stdout/stderr beyond pipe capacity, partial reads, no loss\n";
        for (bool stdout_only : {true, false}) {
            Harness test(helper, 1);
            auto options = test.options("stdio-small");
            (stdout_only ? options.stderr_mode : options.stdout_mode) = StdioMode::Null;
            auto child = test.spawn(options);
            require(child.stdout_reader.has_value() == stdout_only &&
                        child.stderr_reader.has_value() != stdout_only,
                    "capture/null presence mismatch");
            const auto bytes = test.drain(child);
            require(bytes[0] == (stdout_only ? expected_out : expected_err), "single capture failed");
            require(take(child.process.wait_until(soon())).value == 0, "single capture helper failed");
        }

        for (auto out : {StdioMode::Inherit, StdioMode::Null, StdioMode::Capture}) {
            for (auto err : {StdioMode::Inherit, StdioMode::Null, StdioMode::Capture}) {
                Harness test(helper);
                auto options = test.options("exit");
                options.arguments.push_back("0");
                options.stdout_mode = out;
                options.stderr_mode = err;
                auto child = test.spawn(options);
                require(child.stdout_reader.has_value() == (out == StdioMode::Capture) &&
                            child.stderr_reader.has_value() == (err == StdioMode::Capture),
                        "stdio mode mismatch");
                for (const auto& bytes : test.drain(child)) {
                    require(bytes.empty(), "unexpected exit output");
                }
                require(take(child.process.wait_until(soon())).value == 0, "mode helper failed");
            }
        }

        std::cout << "PASS: independent mode matrix and one-credit capture\n";
        {
            Harness test(helper, 2, 1);
            auto options = test.options("stdio-small");
            auto plain = test.processes.spawn(options);
            require(!plain && plain.error().code == ErrorCode::InvalidArgument,
                    "plain spawn accepted capture");
            auto invalid = options;
            invalid.stdin_mode = StdioMode::Capture;
            auto rejected = spawn_with_output(test.processes, test.pipes, invalid);
            require(!rejected && rejected.error().code == ErrorCode::InvalidArgument,
                    "stdin capture accepted");
            invalid = options;
            invalid.stderr_mode = static_cast<StdioMode>(99);
            rejected = spawn_with_output(test.processes, test.pipes, invalid);
            require(!rejected && rejected.error().code == ErrorCode::InvalidArgument,
                    "invalid mode accepted");
            auto missing = options;
            missing.executable = helper.parent_path() / "missing-stdio-helper";
            for (int i = 0; i < 12; ++i) {
                rejected = spawn_with_output(test.processes, test.pipes, missing);
                require(!rejected && rejected.error().code == ErrorCode::NotFound,
                        "spawn failure leaked capacity");
            }

            auto child = test.spawn(options);
            require(test.drain(child)[0] == expected_out, "capture did not recover after failure");
            require(take(child.process.wait_until(soon())).value == 0, "recovered child failed");
        }
        {
            Harness test(helper, 1);
            auto rejected = spawn_with_output(test.processes, test.pipes, test.options("stdio-small"));
            require(!rejected && rejected.error().code == ErrorCode::ResourceExhausted,
                    "capture exceeded capacity");
        }

        std::cout << "PASS: invalid modes, capacity rejection and transactional spawn rollback\n";
        {
            Harness test(helper);
            auto options = test.options("sleep");
            options.arguments.push_back("30000");
            auto child = test.spawn(options);
            take(test.pipes.submit_read(*child.stdout_reader, IoBuffer(8),
                                        std::chrono::steady_clock::now() + 30ms));
            require(test.next().status == IoStatus::TimedOut, "read deadline ignored");
            auto id = take(test.pipes.submit_read(*child.stdout_reader, IoBuffer(8), soon()));
            take(test.pipes.cancel(id));
            require(test.next().status == IoStatus::Cancelled, "read cancel ignored");
            take(test.pipes.submit_read(*child.stderr_reader, IoBuffer(8), soon()));
            child.stderr_reader->close();
            require(test.next().status == IoStatus::Closed, "reader close did not finish pending I/O");
            require(!take(child.process.try_wait()), "reader close terminated sleeping child");
            take(test.pipes.submit_read(*child.stdout_reader, IoBuffer(8), soon()));
            take(child.process.terminate());
            take(child.process.wait_until(soon()));
            require(test.next().status == IoStatus::EndOfStream, "terminated child retained writer");
        }
        {
            Harness test(helper);
            auto child = test.spawn(test.options("stdio-close"));
            const auto bytes = test.drain(child);
            require(bytes[0].empty() && bytes[1].empty(), "unexpected closed-stream data");
            require(!take(child.process.try_wait()), "EOF incorrectly implies process exit");
        }
        {
            Harness test(helper);
            auto child = test.spawn(test.options("stdio-tail-abort"));
            const auto bytes = test.drain(child);
            require(bytes[0] == expected_out && bytes[1] == expected_err, "crash lost flushed output");
            const auto exit = take(child.process.wait_until(soon()));
            require(exit.kind != ExitKind::Code || exit.value != 0, "abort exited successfully");
        }

        std::cout << "PASS: timeout/cancel/close, EOF before exit, termination and crash output\n";
        {
            Harness test(pipe_helper, 4);
            auto pipe = take(test.pipes.create_pipe());
            const std::array<PipeBinding, 1> bindings{{{42, &pipe.writer}}};
            auto options = test.options("single-source");
            auto missing = options;
            missing.executable = pipe_helper.parent_path() / "missing-stdio-helper";
            for (int i = 0; i < 4; ++i) {
                require(!spawn_with_output(test.processes, test.pipes, missing, bindings),
                        "missing helper started");
            }

            auto child = test.spawn(options, bindings);
            auto write = test.pipes.submit_write(pipe.writer, IoBuffer(1), soon());
            require(!write && write.error().code == ErrorCode::Closed, "handed-off writer stayed usable");
            std::array<PipeReader*, 3> readers{&*child.stdout_reader, &*child.stderr_reader, &pipe.reader};
            const auto bytes = test.drain(readers);
            require(bytes[0].empty() && bytes[1].empty() && bytes[2] == "*",
                    "stdio and bootstrap handoff interfered");
            require(take(child.process.wait_until(soon())).value == 0, "combined helper failed");
        }

        std::cout << "PASS: capture composes with transactional generic Pipe handoff\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
