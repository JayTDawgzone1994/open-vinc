#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace vinc::osal;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible_v<PipeReader>);
static_assert(!std::is_copy_constructible_v<IoBuffer>);
static_assert(std::is_nothrow_move_constructible_v<PipeReader>);
static_assert(std::is_nothrow_move_assignable_v<IoCompletion>);
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error("OSAL " + std::to_string(static_cast<int>(result.error().code)) + " op " +
        std::to_string(static_cast<int>(result.error().operation)) + ": " + result.error().native.message());
    return std::move(*result);
}
void ok(Result<void> result) {
    if (!result) throw std::runtime_error("OSAL " + std::to_string(static_cast<int>(result.error().code)) + ": " + result.error().native.message());
}
Deadline soon() { return std::chrono::steady_clock::now() + 10s; }
struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes;
    std::filesystem::path helper;
    std::vector<ProcessId> ids;
    Harness(std::filesystem::path path, PipeServiceOptions options = {})
        : pipes(take(PipeService::create(options))), helper(std::move(path)) {}
    ~Harness() {
        for (auto id : ids) { auto ignored = processes.terminate(id); (void)ignored; }
        auto ignored = pipes.shutdown_until(soon()); (void)ignored;
        ignored = processes.shutdown_until(soon());
    }
    SpawnOptions options(const std::string& mode) const {
        SpawnOptions result;
        result.executable = helper; result.arguments = {mode};
        result.stdout_mode = result.stderr_mode = StdioMode::Null;
        return result;
    }
    ProcessWithPipe spawn(const std::string& mode) {
        auto result = take(spawn_with_pipe(processes, pipes, options(mode)));
        ids.push_back(result.process.id());
        return result;
    }
    void stop(ProcessWithPipe& child) {
        take(child.process.terminate());
        take(child.process.wait_until(soon()));
        child.pipe.close();
    }
    IoCompletion next() {
        std::array<IoCompletion, 1> output;
        require(take(pipes.wait_completions(output, soon())) == 1, "missing completion");
        return std::move(output[0]);
    }
};

void round_trip(Harness& test, ProcessWithPipe& child, std::size_t size) {
    std::vector<std::byte> expected(size);
    for (std::size_t i = 0; i < size; ++i) expected[i] = static_cast<std::byte>((i * 37 + 11) & 255);
    std::size_t sent = 0, received = 0;
    bool writing = false, reading = false;
    while (received < size || sent < size) {
        if (!writing && sent < size) {
            const auto count = std::min<std::size_t>(32768, size - sent);
            IoBuffer buffer(count);
            std::copy_n(expected.begin() + static_cast<std::ptrdiff_t>(sent), count, buffer.bytes().begin());
            take(test.pipes.submit_write(child.pipe.writer, std::move(buffer), soon()));
            writing = true;
        }
        if (!reading && received < size) {
            IoBuffer buffer(std::min<std::size_t>(997, size - received));
            take(test.pipes.submit_read(child.pipe.reader, std::move(buffer), soon()));
            reading = true;
        }
        auto result = test.next();
        require(result.status == IoStatus::Completed && result.transferred > 0, "round trip I/O failed");
        require(result.pipe == (result.kind == IoKind::Read ? child.pipe.reader.id() : child.pipe.writer.id()), "wrong endpoint identity");
        if (result.kind == IoKind::Write) { sent += result.transferred; writing = false; }
        else {
            require(result.transferred <= size - received, "extra bytes");
            require(std::equal(result.buffer.bytes().begin(), result.buffer.bytes().begin() + static_cast<std::ptrdiff_t>(result.transferred),
                expected.begin() + static_cast<std::ptrdiff_t>(received)), "stream bytes corrupted");
            received += result.transferred; reading = false;
        }
    }
    child.pipe.close();
    const auto status = take(child.process.wait_until(soon()));
    require(status.kind == ExitKind::Code && status.value == 0, "helper did not observe endpoint close");
}

int tests(const std::filesystem::path& helper) {
    {
        Harness test(helper);
        auto first_in = take(test.pipes.create_pipe());
        auto first_out = take(test.pipes.create_pipe());
        auto second_in = take(test.pipes.create_pipe());
        auto second_out = take(test.pipes.create_pipe());
        // Deliberately unordered, arbitrary slot identities and both directions.
        const std::array<PipeBinding, 4> bindings{{{99, &second_out.writer}, {17, &first_in.reader},
            {33, &first_out.writer}, {71, &second_in.reader}}};
        auto child = take(spawn_with_pipes(test.processes, test.pipes, test.options("multi"), bindings));
        test.ids.push_back(child.id());
        auto closed = test.pipes.submit_read(first_in.reader, IoBuffer(1), soon());
        require(!closed && closed.error().code == ErrorCode::Closed, "handoff kept local end usable");
        for (auto ends : {std::pair{&first_in.writer, &first_out.reader}, {&second_in.writer, &second_out.reader}}) {
            IoBuffer byte(1); byte.bytes()[0] = std::byte{42};
            take(test.pipes.submit_write(*ends.first, std::move(byte), soon()));
            require(test.next().status == IoStatus::Completed, "multi write");
            ends.first->close();
            std::vector<std::byte> received;
            for (;;) {
                take(test.pipes.submit_read(*ends.second, IoBuffer(1), soon()));
                auto result = test.next();
                if (result.status == IoStatus::EndOfStream) break;
                require(result.status == IoStatus::Completed && result.transferred == 1 && received.size() < 2, "multi read");
                received.push_back(result.buffer.bytes()[0]);
            }
            require(received == std::vector<std::byte>{std::byte{42}, std::byte{0xee}}, "half-close lost reverse response");
        }
        require(take(child.wait_until(soon())).value == 0, "multi child failed");
    }
    std::cout << "PASS: precreated pipes, arbitrary slots, multi-end handoff, reverse traffic after EOF\n";
    {
        Harness test(helper);
        auto pipe = take(test.pipes.create_pipe());
        const std::array<PipeBinding, 1> binding{{{42, &pipe.reader}}};
        auto missing = test.options("single-sink"); missing.executable = helper.parent_path() / "missing-generic-pipe-helper";
        for (int i = 0; i < 4; ++i) require(!spawn_with_pipes(test.processes, test.pipes, missing, binding), "missing generic helper started");
        const std::array<PipeBinding, 2> duplicate_slot{{{42, &pipe.reader}, {42, &pipe.writer}}};
        auto duplicate = spawn_with_pipes(test.processes, test.pipes, missing, duplicate_slot);
        require(!duplicate && duplicate.error().code == ErrorCode::InvalidArgument, "duplicate slot admitted");
        const std::array<PipeBinding, 2> duplicate_end{{{1, &pipe.reader}, {2, &pipe.reader}}};
        duplicate = spawn_with_pipes(test.processes, test.pipes, missing, duplicate_end);
        require(!duplicate && duplicate.error().code == ErrorCode::InvalidArgument, "duplicate end admitted");
        auto foreign = take(PipeService::create());
        auto wrong = spawn_with_pipes(test.processes, foreign, missing, binding);
        require(!wrong && wrong.error().code == ErrorCode::InvalidArgument, "foreign service binding accepted");
        // The untouched read end remains exportable despite buffered data.
        IoBuffer byte(1); byte.bytes()[0] = std::byte{42};
        take(test.pipes.submit_write(pipe.writer, std::move(byte), soon()));
        require(test.next().status == IoStatus::Completed, "failed spawn damaged pipe");
        auto child = take(spawn_with_pipes(test.processes, test.pipes, test.options("single-sink"), binding));
        test.ids.push_back(child.id());
        pipe.writer.close();
        require(take(child.wait_until(soon())).value == 0, "reader handoff lost data/EOF");
        ok(foreign.shutdown_until(soon()));
    }
    std::cout << "PASS: failed handoff preserves ends/data, validation, reader-only child\n";
    {
        Harness test(helper);
        auto pipe = take(test.pipes.create_pipe());
        auto read = take(test.pipes.submit_read(pipe.reader, IoBuffer(1), soon()));
        const std::array<PipeBinding, 1> binding{{{42, &pipe.reader}}};
        auto busy = spawn_with_pipes(test.processes, test.pipes, test.options("single-sink"), binding);
        require(!busy && busy.error().code == ErrorCode::Busy, "active endpoint handed off");
        take(test.pipes.cancel(read)); require(test.next().status == IoStatus::Cancelled, "cancel handoff probe");
        busy = spawn_with_pipes(test.processes, test.pipes, test.options("single-sink"), binding);
        require(!busy && busy.error().code == ErrorCode::Busy, "used endpoint handed off");
        pipe.reader.close();
        auto closed = spawn_with_pipes(test.processes, test.pipes, test.options("single-sink"), binding);
        require(!closed && closed.error().code == ErrorCode::Closed, "closed end handed off");
    }
    std::cout << "PASS: active/previously used/closed end handoff rejection\n";
    {
        Harness test(helper);
        std::vector<Pipe> pipes;
        std::vector<PipeBinding> bindings;
        pipes.reserve(64); bindings.reserve(65);
        for (std::uint32_t i = 0; i < 64; ++i) {
            pipes.push_back(take(test.pipes.create_pipe()));
            bindings.push_back({1000 + i, &pipes.back().writer});
        }
        bindings.push_back({5000, &pipes[0].reader});
        auto overflow = spawn_with_pipes(test.processes, test.pipes, test.options("many"), bindings);
        require(!overflow && overflow.error().code == ErrorCode::InvalidArgument, "binding count not bounded");
        bindings.pop_back();
        auto child = take(spawn_with_pipes(test.processes, test.pipes, test.options("many"), bindings));
        test.ids.push_back(child.id());
        for (std::size_t i = 0; i < pipes.size(); ++i) {
            take(test.pipes.submit_read(pipes[i].reader, IoBuffer(1), soon()));
            auto result = test.next();
            require(result.status == IoStatus::Completed && result.transferred == 1 &&
                result.buffer.bytes()[0] == static_cast<std::byte>((1000 + i) & 255), "64-end handoff mapping");
            take(test.pipes.submit_read(pipes[i].reader, IoBuffer(1), soon()));
            require(test.next().status == IoStatus::EndOfStream, "parent retained writer copy");
        }
        require(take(child.wait_until(soon())).value == 0, "many writer child failed");
    }
    std::cout << "PASS: 64-end inheritance whitelist, slot mapping, EOF, count bound\n";
    {
        Harness test(helper);
        auto pipe = take(test.pipes.create_pipe());
        const std::array<PipeBinding, 1> source_binding{{{42, &pipe.writer}}};
        const std::array<PipeBinding, 1> sink_binding{{{42, &pipe.reader}}};
        auto source = take(spawn_with_pipes(test.processes, test.pipes, test.options("single-source"), source_binding));
        test.ids.push_back(source.id());
        auto sink = take(spawn_with_pipes(test.processes, test.pipes, test.options("single-sink"), sink_binding));
        test.ids.push_back(sink.id());
        require(take(source.wait_until(soon())).value == 0 && take(sink.wait_until(soon())).value == 0,
            "pipe between two children failed");
    }
    std::cout << "PASS: one pipe directly connects two independent child processes\n";
    {
        Harness test(helper);
        auto private_pipe = take(test.pipes.create_pipe());
        auto child = test.spawn("hold");
        private_pipe.writer.close();
        take(test.pipes.submit_read(private_pipe.reader, IoBuffer(1), soon()));
        require(test.next().status == IoStatus::EndOfStream, "unlisted writer leaked to child");
        require(!take(child.process.try_wait()), "whitelist test child exited early");
        test.stop(child);
    }
    std::cout << "PASS: unrelated pipe ends do not leak into spawned children\n";
    {
        Harness test(helper);
        for (int i = 0; i < 12; ++i) {
            auto service = take(PipeService::create());
            auto pipe = take(service.create_pipe());
            const std::array<PipeBinding, 1> binding{{{42, &pipe.writer}}};
            std::latch start(2);
            std::optional<Result<Process>> launched;
            std::jthread starter([&] {
                start.arrive_and_wait();
                launched = spawn_with_pipes(test.processes, service, test.options("single-source"), binding);
            });
            start.arrive_and_wait();
            auto stopped = service.shutdown_until(soon());
            starter.join();
            if (*launched) {
                auto child = std::move(**launched);
                test.ids.push_back(child.id());
                take(child.terminate()); take(child.wait_until(soon()));
            } else require(launched->error().code == ErrorCode::ServiceClosed || launched->error().code == ErrorCode::Closed,
                "unexpected handoff/shutdown result");
            ok(stopped);
        }
    }
    std::cout << "PASS: shutdown racing endpoint handoff preserves process ownership\n";
    {
        Harness test(helper);
        auto first = take(test.pipes.create_pipe());
        auto second = take(test.pipes.create_pipe());
        const std::array<PipeBinding, 2> bindings{{{7, &first.writer}, {8, &second.writer}}};
        auto child = take(spawn_with_pipes(test.processes, test.pipes, test.options("reject-capacity"), bindings));
        test.ids.push_back(child.id());
        for (auto* reader : {&first.reader, &second.reader}) {
            take(test.pipes.submit_read(*reader, IoBuffer(1), soon()));
            require(test.next().status == IoStatus::EndOfStream, "failed adoption leaked native end");
        }
        require(!take(child.try_wait()), "failed adoption helper exited unexpectedly");
        take(child.terminate()); take(child.wait_until(soon()));
    }
    std::cout << "PASS: child adoption failure closes all claimed ends while child stays alive\n";
    {
        Harness test(helper);
        auto child = test.spawn("delayed-echo");
        round_trip(test, child, 512 * 1024 + 17);
        require(!test.pipes.monitor_error(), "monitor failed");
    }
    std::cout << "PASS: duplex stream, partial reads/writes, delayed peer, endpoint ownership\n";
    {
        Harness test(helper);
        auto child = test.spawn("hold");
        IoBuffer buffer(16);
        auto id = take(test.pipes.submit_read(child.pipe.reader, std::move(buffer), soon()));
        require(buffer.size() == 0, "accepted buffer not transferred");
        IoBuffer rejected(16);
        auto busy = test.pipes.submit_read(child.pipe.reader, std::move(rejected), soon());
        require(!busy && busy.error().code == ErrorCode::Busy && rejected.size() == 16, "busy consumed buffer");
        std::array<IoCompletion, 1> output;
        auto timed = test.pipes.wait_completions(output, std::chrono::steady_clock::now() + 20ms);
        require(!timed && timed.error().code == ErrorCode::Timeout, "completion wait timeout");
        std::stop_source cancel_wait;
        cancel_wait.request_stop();
        auto cancelled = test.pipes.wait_completions(output, soon(), cancel_wait.get_token());
        require(!cancelled && cancelled.error().code == ErrorCode::Cancelled, "completion wait cancellation");
        require(take(test.pipes.cancel(id)) == CancelResult::Requested, "wait cancellation changed I/O");
        auto result = test.next();
        require(result.id == id && result.status == IoStatus::Cancelled && result.buffer.size() == 16, "cancel did not return storage");
        auto expired = take(test.pipes.submit_read(child.pipe.reader, std::move(result.buffer), std::chrono::steady_clock::now() + 20ms));
        result = test.next();
        require(result.id == expired && result.status == IoStatus::TimedOut, "I/O deadline");
        auto past = take(test.pipes.submit_read(child.pipe.reader, std::move(result.buffer), Deadline::min()));
        result = test.next();
        require(result.id == past && result.status == IoStatus::TimedOut, "past deadline");
        auto pending = take(test.pipes.submit_read(child.pipe.reader, std::move(result.buffer), soon()));
        child.pipe.close();
        result = test.next();
        require(result.id == pending && result.status == IoStatus::Closed, "local close lost pending I/O");
        require(!take(child.process.try_wait()), "pipe close killed process");
        test.stop(child);
    }
    std::cout << "PASS: Busy, buffer ownership, wait cancellation, I/O cancellation, deadlines, local close\n";
    {
        Harness test(helper);
        auto child = test.spawn("close");
        IoBuffer buffer(1);
        take(test.pipes.submit_read(child.pipe.reader, std::move(buffer), soon()));
        require(test.next().status == IoStatus::EndOfStream, "peer close did not produce EOF");
        require(!take(child.process.try_wait()), "EOF mistaken for process exit");
        buffer = IoBuffer(1);
        take(test.pipes.submit_write(child.pipe.writer, std::move(buffer), soon()));
        require(test.next().status == IoStatus::PeerClosed, "broken pipe write not reported");
        test.stop(child);
    }
    std::cout << "PASS: peer close, EOF independent of process exit, broken-pipe write\n";
    {
        Harness test(helper);
        auto child = test.spawn("hold");
        IoBuffer buffer(1);
        take(test.pipes.submit_read(child.pipe.reader, std::move(buffer), soon()));
        take(child.process.terminate());
        take(child.process.wait_until(soon()));
        require(test.next().status == IoStatus::EndOfStream, "dead child endpoint leaked in parent");
    }
    std::cout << "PASS: killed peer, no inherited endpoint retained in parent\n";
    {
        PipeServiceOptions limits;
        limits.max_endpoints = 4; limits.max_operations = 1;
        limits.max_buffered_bytes = 8; limits.max_transfer_bytes = 16;
        Harness test(helper, limits);
        auto missing = test.options("hold"); missing.executable = helper.parent_path() / "vinc-no-such-helper";
        for (int i = 0; i < 4; ++i) require(!spawn_with_pipe(test.processes, test.pipes, missing), "missing executable created");
        auto child = test.spawn("hold"); // failed starts must release capacity immediately
        auto full = spawn_with_pipe(test.processes, test.pipes, test.options("hold"));
        require(!full && full.error().code == ErrorCode::ResourceExhausted, "endpoint bound");
        IoBuffer too_large(17);
        auto invalid = test.pipes.submit_read(child.pipe.reader, std::move(too_large), soon());
        require(!invalid && invalid.error().code == ErrorCode::InvalidArgument && too_large.size() == 17, "transfer size bound");
        IoBuffer over_budget(9);
        auto budget = test.pipes.submit_read(child.pipe.reader, std::move(over_budget), soon());
        require(!budget && budget.error().code == ErrorCode::ResourceExhausted && over_budget.size() == 9, "byte budget");
        IoBuffer buffer(8);
        auto id = take(test.pipes.submit_read(child.pipe.reader, std::move(buffer), Deadline::min()));
        IoBuffer write(1);
        auto no_slot = test.pipes.submit_write(child.pipe.writer, std::move(write), soon());
        require(!no_slot && no_slot.error().code == ErrorCode::ResourceExhausted, "completion credit not reserved");
        require(test.next().id == id, "completion lost at capacity");
        take(test.pipes.submit_read(child.pipe.reader, IoBuffer(8), soon()));
        ok(test.pipes.shutdown_until(soon()));
        require(test.next().status == IoStatus::Closed, "shutdown discarded completion");
        buffer = IoBuffer(1);
        auto closed = test.pipes.submit_read(child.pipe.reader, std::move(buffer), soon());
        require(!closed && closed.error().code == ErrorCode::ServiceClosed && buffer.size() == 1, "closed admission");
        test.stop(child);
    }
    std::cout << "PASS: failed-spawn rollback, endpoint/operation/byte bounds, shutdown drain\n";
    {
        Harness test(helper);
        auto child = test.spawn("descendant");
        round_trip(test, child, 137);
        auto raw_options = test.options("no-bootstrap");
        auto raw = take(test.processes.spawn(raw_options));
        test.ids.push_back(raw.id());
        require(take(raw.wait_until(soon())).value == 0, "plain spawn inherited pipe bootstrap");
        auto spoofed = test.options("hold");
        spoofed.environment = {{"VINC_OSAL_PIPE_BOOTSTRAP", "bad"}};
        require(!spawn_with_pipe(test.processes, test.pipes, spoofed), "reserved bootstrap overridable");
    }
    std::cout << "PASS: one-time child adoption, descendants and plain spawn have no bootstrap\n";
    {
        Harness test(helper);
        std::vector<std::optional<Result<ProcessWithPipe>>> children(8);
        std::vector<std::jthread> starters;
        for (std::size_t i = 0; i < children.size(); ++i)
            starters.emplace_back([&, i] { children[i] = spawn_with_pipe(test.processes, test.pipes, test.options("hold")); });
        starters.clear();
        std::vector<ProcessWithPipe> live;
        for (auto& result : children) {
            live.push_back(take(std::move(*result)));
            test.ids.push_back(live.back().process.id());
        }
        std::vector<IoId> operations;
        for (auto& child : live) operations.push_back(take(test.pipes.submit_read(child.pipe.reader, IoBuffer(4), soon())));
        std::vector<std::jthread> cancellers;
        for (auto id : operations) cancellers.emplace_back([&, id] { auto result = test.pipes.cancel(id); (void)result; });
        cancellers.clear();
        std::vector<IoId> completed;
        std::array<IoCompletion, 8> batch;
        while (completed.size() != live.size()) {
            auto count = take(test.pipes.wait_completions(batch, soon()));
            for (std::size_t i = 0; i < count; ++i) {
                require(batch[i].status == IoStatus::Cancelled, "concurrent cancel result");
                require(std::find(completed.begin(), completed.end(), batch[i].id) == completed.end(), "duplicate completion");
                completed.push_back(batch[i].id);
            }
        }
        for (auto& child : live) test.stop(child);
        require(!test.pipes.monitor_error(), "native monitor failure");
    }
    std::cout << "PASS: concurrent spawn and cancellation, batch completion, unique operation IDs\n";
    {
        Harness test(helper);
        auto child = test.spawn("hold");
        for (int i = 0; i < 24; ++i) {
            auto id = take(test.pipes.submit_write(child.pipe.writer, IoBuffer(1), soon()));
            std::jthread canceller([&] { auto ignored = test.pipes.cancel(id); (void)ignored; });
            auto result = test.next();
            require(result.id == id && (result.status == IoStatus::Completed || result.status == IoStatus::Cancelled), "completion/cancel race");
        }
        std::array<IoCompletion, 1> output;
        auto empty = test.pipes.wait_completions(output, std::chrono::steady_clock::now() + 20ms);
        require(!empty && empty.error().code == ErrorCode::Timeout, "race produced duplicate completion");
        auto moved = std::move(child.pipe.reader);
        require(!child.pipe.reader && moved, "endpoint move");
        auto id = take(test.pipes.submit_read(moved, IoBuffer(16), soon()));
        moved = PipeReader{};
        auto result = test.next();
        require(result.id == id && result.status == IoStatus::Closed, "endpoint move assignment lost pending I/O");
        test.stop(child);
    }
    std::cout << "PASS: cancellation/completion races and move/destruction with pending I/O\n";
    {
        Harness test(helper);
        auto child = test.spawn("hold");
        std::size_t total = 0;
        bool stalled = false;
        for (int i = 0; i < 64 && !stalled; ++i) {
            IoBuffer buffer(65536);
            take(test.pipes.submit_write(child.pipe.writer, std::move(buffer), std::chrono::steady_clock::now() + 30ms));
            auto result = test.next();
            total += result.transferred;
            if (result.status == IoStatus::TimedOut) stalled = true;
            else require(result.status == IoStatus::Completed, "unexpected backpressure result");
        }
        (void)total;
        require(stalled, "non-reading peer never applied backpressure");
        test.stop(child);
    }
    std::cout << "PASS: non-reading peer applies bounded backpressure and write deadline\n";
    {
        Harness test(helper);
        ProcessWithPipe child;
        {
            auto transient = take(PipeService::create());
            child = take(spawn_with_pipe(test.processes, transient, test.options("hold")));
            test.ids.push_back(child.process.id());
            take(transient.submit_read(child.pipe.reader, IoBuffer(16), soon()));
        }
        child.pipe.close();
        test.stop(child);
    }
    std::cout << "PASS: service facade destruction retains pending native I/O storage\n";
    {
        Harness test(helper);
        { auto warm = test.spawn("echo"); round_trip(test, warm, 1); }
#ifdef _WIN32
        DWORD before = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count before churn");
#endif
        for (int i = 0; i < 20; ++i) {
            auto child = test.spawn("echo");
            round_trip(test, child, 31);
            ok(test.processes.forget(child.process.id()));
        }
        ok(test.pipes.shutdown_until(soon()));
#ifdef _WIN32
        DWORD after = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "handle count after churn");
        // Allow small thread-pool bookkeeping changes; a per-launch handle leak
        // would exceed this allowance after twenty complete lifecycles.
        require(after <= before + 4, "native handles leaked across repeated pipe lifecycles");
#endif
    }
    std::cout << "PASS: repeated pipe lifecycle and native resource collection\n";
    return 0;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected pipe helper path");
        return tests(std::filesystem::absolute(argv[1]));
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
