// Process-independent PipeReader/PipeWriter tests. Verify one-way stream behavior
// and ownership/limits without involving child bootstrap or module policy.

// Intentionally no process.hpp/process_pipe.hpp: Pipe works independently.
#include <vinc/time.hpp>
#include <vinc/osal/pipe.hpp>
#include <algorithm>
#include <array>
#include <iostream>
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

namespace {
// Normal I/O and shutdown waits have a bounded test deadline.
constexpr auto operation_timeout = 5s;
} // namespace

template <class T>
concept Readable = requires(PipeService& service, T& endpoint) {
    // Check reader compatibility at compile time without submitting real I/O.
    service.submit_read(endpoint, IoBuffer(1), vinc::Deadline{});
};
template <class T>
concept Writable = requires(PipeService& service, T& endpoint) {
    // Check writer compatibility at compile time without submitting real I/O.
    service.submit_write(endpoint, IoBuffer(1), vinc::Deadline{});
};
static_assert(Readable<PipeReader> && !Readable<PipeWriter>);
static_assert(Writable<PipeWriter> && !Writable<PipeReader>);
static_assert(!std::is_copy_constructible_v<PipeReader> && !std::is_copy_constructible_v<PipeWriter>);
static_assert(std::is_nothrow_move_constructible_v<Pipe> && std::is_nothrow_move_assignable_v<Pipe>);

void require(bool ok, const char* text) {
    if (!ok) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void check(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error("shutdown failed");
    }
}

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

IoCompletion next(PipeService& service) {
    std::array<IoCompletion, 1> completions;
    require(take(service.wait_completions(completions, soon())) == 1, "missing completion");
    return std::move(completions[0]);
}

void stream_transfer_and_eof() {
    auto service = take(PipeService::create());
    auto pipe = take(service.create_pipe());
    require(pipe.reader.id() != pipe.writer.id(), "ends share identity");
    // Unequal chunk sizes and a trailing fragment exercise partial transfers.
    constexpr std::size_t payload_bytes = 1024 * 1024 + 17;
    constexpr std::size_t write_chunk_bytes = 32768;
    constexpr std::size_t read_chunk_bytes = 997;
    std::vector<std::byte> expected(payload_bytes);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<std::byte>((i * 17 + 41) & 255);
    }

    std::size_t sent = 0;
    std::size_t received = 0;
    bool write_pending = false;
    bool read_pending = false;
    bool reached_eof = false;
    while (!reached_eof) {
        if (!write_pending && sent < expected.size()) {
            const auto count = std::min<std::size_t>(write_chunk_bytes, expected.size() - sent);
            IoBuffer buffer(count);
            std::copy_n(expected.data() + sent, count, buffer.bytes().data());
            take(service.submit_write(pipe.writer, std::move(buffer), soon()));
            write_pending = true;
        }

        if (!read_pending) {
            take(service.submit_read(pipe.reader, IoBuffer(read_chunk_bytes), soon()));
            read_pending = true;
        }

        auto result = next(service);
        if (result.kind == IoKind::Write) {
            require(result.pipe == pipe.writer.id() && result.status == IoStatus::Completed &&
                        result.transferred,
                    "write failed");
            sent += result.transferred;
            write_pending = false;
            if (sent == expected.size()) {
                pipe.writer.close();
            }
        } else {
            require(result.pipe == pipe.reader.id(), "reader identity");
            read_pending = false;
            if (result.status == IoStatus::EndOfStream) {
                reached_eof = true;
                continue;
            }

            require(result.status == IoStatus::Completed && result.transferred &&
                        result.transferred <= expected.size() - received,
                    "read failed");
            require(
                std::equal(result.buffer.bytes().begin(),
                           result.buffer.bytes().begin() + static_cast<std::ptrdiff_t>(result.transferred),
                           expected.begin() + static_cast<std::ptrdiff_t>(received)),
                "corrupt bytes");
            received += result.transferred;
        }
    }

    require(sent == expected.size() && received == expected.size(), "EOF lost buffered bytes");

    check(service.shutdown_until(soon()));
}

void independent_directions_and_peer_close() {
    constexpr int reverse_io_completions = 2; // One read and one write.
    auto service = take(PipeService::create());
    auto forward = take(service.create_pipe());
    auto reverse = take(service.create_pipe());
    forward.writer.close();
    take(service.submit_read(forward.reader, IoBuffer(1), soon()));
    require(next(service).status == IoStatus::EndOfStream, "one-way EOF");
    IoBuffer data(1);
    data.bytes()[0] = std::byte{79};
    take(service.submit_read(reverse.reader, IoBuffer(1), soon()));
    take(service.submit_write(reverse.writer, std::move(data), soon()));
    for (int i = 0; i < reverse_io_completions; ++i) {
        auto result = next(service);
        require(result.status == IoStatus::Completed && result.transferred == 1,
                "reverse direction was closed");
        if (result.kind == IoKind::Read) {
            require(result.buffer.bytes()[0] == std::byte{79}, "reverse data");
        }
    }

    reverse.reader.close();

    // EOF on a separate marker proves all earlier close requests have
    // been processed; no timing sleeps are needed for PeerClosed.
    auto marker = take(service.create_pipe());
    marker.writer.close();
    take(service.submit_read(marker.reader, IoBuffer(1), soon()));
    require(next(service).status == IoStatus::EndOfStream, "marker EOF");
    take(service.submit_write(reverse.writer, IoBuffer(1), soon()));
    require(next(service).status == IoStatus::PeerClosed, "closed reader not reported");

    check(service.shutdown_until(soon()));
}

void capacity_and_reader_ownership() {
    // One endpoint credit remains, which is insufficient for another complete pair.
    constexpr std::size_t endpoint_limit = 3;
    constexpr int rejected_pair_attempts = 8;
    constexpr std::size_t ownership_probe_bytes = 7;
    PipeServiceOptions options;
    options.max_endpoints = endpoint_limit;
    auto service = take(PipeService::create(options));
    auto pipe = take(service.create_pipe());
    for (int i = 0; i < rejected_pair_attempts; ++i) {
        auto rejected = service.create_pipe();
        require(!rejected && rejected.error().is(vinc::osal::ErrorCode::ResourceExhausted),
                "partial pair admitted");
    }

    auto other = take(PipeService::create());
    IoBuffer buffer(ownership_probe_bytes);
    auto wrong = other.submit_read(pipe.reader, std::move(buffer), soon());
    require(!wrong && wrong.error().is(vinc::osal::ErrorCode::InvalidArgument) &&
                buffer.size() == ownership_probe_bytes,
            "wrong service consumed buffer");
    auto moved = std::move(pipe.reader);
    require(!pipe.reader && moved, "reader ownership move");
    auto pending = take(service.submit_read(moved, std::move(buffer), soon()));
    moved = PipeReader{};
    auto result = next(service);
    require(result.id == pending && result.status == IoStatus::Closed &&
                result.buffer.size() == ownership_probe_bytes,
            "reader destruction lost I/O");

    check(service.shutdown_until(soon()));
    check(other.shutdown_until(soon()));
}

void cancellation_deadlines_and_backpressure() {
    constexpr std::size_t cancelled_read_bytes = 4;
    // Stop after this many writes even if the backend unexpectedly keeps accepting data.
    constexpr int maximum_backpressure_writes = 128;
    constexpr std::size_t backpressure_chunk_bytes = 65536;
    constexpr auto backpressure_write_timeout = 30ms;
    auto service = take(PipeService::create());
    auto pipe = take(service.create_pipe());
    auto id = take(service.submit_read(pipe.reader, IoBuffer(cancelled_read_bytes), soon()));
    std::stop_source stop;
    std::jthread stopper([&] {
        stop.request_stop();
    });
    std::array<IoCompletion, 1> output;
    auto cancelled_wait = service.wait_completions(output, soon(), stop.get_token());
    require(!cancelled_wait && cancelled_wait.error().is(vinc::osal::ErrorCode::Cancelled),
            "wait not cancelled");
    take(service.cancel(id));
    auto result = next(service);
    require(result.id == id && result.status == IoStatus::Cancelled, "read cancellation");
    take(service.submit_read(pipe.reader, std::move(result.buffer), vinc::Deadline::min()));
    require(next(service).status == IoStatus::TimedOut, "expired read");
    bool stalled = false;
    for (int i = 0; i < maximum_backpressure_writes && !stalled; ++i) {
        take(service.submit_write(pipe.writer, IoBuffer(backpressure_chunk_bytes),
                                  std::chrono::steady_clock::now() + backpressure_write_timeout));
        result = next(service);
        stalled = result.status == IoStatus::TimedOut;
        require(stalled || result.status == IoStatus::Completed, "backpressure result");
    }

    require(stalled, "unbounded transport buffer");

    check(service.shutdown_until(soon()));
}

void writer_move_and_shutdown() {
    auto service = take(PipeService::create());
    auto source = take(service.create_pipe());
    auto target = take(service.create_pipe());
    auto moved = std::move(source.writer);
    require(!source.writer && moved, "writer ownership move");
    auto pending = take(service.submit_read(target.reader, IoBuffer(1), soon()));
    target.writer = std::move(moved); // closes the old target write end
    auto result = next(service);
    require(result.id == pending && result.status == IoStatus::EndOfStream,
            "writer move assignment did not close old end");
    auto read = take(service.submit_read(source.reader, IoBuffer(1), soon()));

    check(service.shutdown_until(soon()));
    require(next(service).id == read, "shutdown lost completion");
    auto rejected = service.create_pipe();
    require(!rejected && rejected.error().is(vinc::osal::ErrorCode::ServiceClosed), "create after shutdown");
}

void repeated_lifecycle_and_cleanup() {
    constexpr int lifecycle_iterations = 50;
#ifdef _WIN32
    // Allow small thread-pool bookkeeping changes, not a per-lifecycle handle leak.
    constexpr DWORD handle_count_allowance = 2;
    DWORD before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count");
#endif
    for (int i = 0; i < lifecycle_iterations; ++i) {
        auto service = take(PipeService::create());
        auto pipe = take(service.create_pipe());
        take(service.submit_read(pipe.reader, IoBuffer(1), soon()));

        check(service.shutdown_until(soon()));
        require(next(service).status == IoStatus::Closed, "pending close");
    }
#ifdef _WIN32
    DWORD after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 &&
                after <= before + handle_count_allowance,
            "standalone handle leak");
#endif
}

int main() {
    try {
        stream_transfer_and_eof();
        std::cout << "PASS: standalone stream, directional types, partial transfers, drain before EOF\n";

        independent_directions_and_peer_close();
        std::cout << "PASS: independent directions, half close by composition, closed reader\n";

        capacity_and_reader_ownership();
        std::cout << "PASS: atomic pair capacity, service identity, move with pending read\n";

        cancellation_deadlines_and_backpressure();
        std::cout << "PASS: wait cancellation, read cancellation/deadline, bounded backpressure\n";

        writer_move_and_shutdown();
        std::cout << "PASS: writer move assignment, shutdown drain and closed admission\n";

        repeated_lifecycle_and_cleanup();
        std::cout << "PASS: standalone service/pipe churn and resource collection\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
