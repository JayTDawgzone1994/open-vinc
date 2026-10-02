// Process-independent PipeReader/PipeWriter tests. Verify one-way stream behavior
// and ownership/limits without involving child bootstrap or module policy.

// Intentionally no process.hpp/process_pipe.hpp: Pipe works independently.
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
template <class T>
concept Readable = requires(PipeService& s, T& p) { s.submit_read(p, IoBuffer(1), Deadline{}); };
template <class T>
concept Writable = requires(PipeService& s, T& p) { s.submit_write(p, IoBuffer(1), Deadline{}); };
static_assert(Readable<PipeReader> && !Readable<PipeWriter>);
static_assert(Writable<PipeWriter> && !Writable<PipeReader>);
static_assert(!std::is_copy_constructible_v<PipeReader> && !std::is_copy_constructible_v<PipeWriter>);
static_assert(std::is_nothrow_move_constructible_v<Pipe> && std::is_nothrow_move_assignable_v<Pipe>);

void require(bool ok, const char* text) {
    if (!ok) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error("OSAL " + std::to_string(static_cast<int>(result.error().code)) + " " +
                                 result.error().native.message());
    }

    return std::move(*result);
}

void check(Result<void> result) {
    if (!result) {
        throw std::runtime_error("shutdown failed");
    }
}

Deadline soon() {
    return std::chrono::steady_clock::now() + 5s;
}

IoCompletion next(PipeService& service) {
    std::array<IoCompletion, 1> out;
    require(take(service.wait_completions(out, soon())) == 1, "missing completion");
    return std::move(out[0]);
}

int main() {
    try {
        {
            auto service = take(PipeService::create());
            auto pipe = take(service.create_pipe());
            require(pipe.reader.id() != pipe.writer.id(), "ends share identity");
            std::vector<std::byte> expected(1024 * 1024 + 17);
            for (std::size_t i = 0; i < expected.size(); ++i) {
                expected[i] = static_cast<std::byte>((i * 17 + 41) & 255);
            }

            std::size_t sent = 0;
            std::size_t received = 0;
            bool writing = false;
            bool reading = false;
            bool ended = false;
            while (!ended) {
                if (!writing && sent < expected.size()) {
                    const auto count = std::min<std::size_t>(32768, expected.size() - sent);
                    IoBuffer buffer(count);
                    std::copy_n(expected.data() + sent, count, buffer.bytes().data());
                    take(service.submit_write(pipe.writer, std::move(buffer), soon()));
                    writing = true;
                }

                if (!reading) {
                    take(service.submit_read(pipe.reader, IoBuffer(997), soon()));
                    reading = true;
                }

                auto result = next(service);
                if (result.kind == IoKind::Write) {
                    require(result.pipe == pipe.writer.id() && result.status == IoStatus::Completed &&
                                result.transferred,
                            "write failed");
                    sent += result.transferred;
                    writing = false;
                    if (sent == expected.size()) {
                        pipe.writer.close();
                    }
                } else {
                    require(result.pipe == pipe.reader.id(), "reader identity");
                    reading = false;
                    if (result.status == IoStatus::EndOfStream) {
                        ended = true;
                        continue;
                    }
                    require(result.status == IoStatus::Completed && result.transferred &&
                                result.transferred <= expected.size() - received,
                            "read failed");
                    require(std::equal(result.buffer.bytes().begin(),
                                       result.buffer.bytes().begin() +
                                           static_cast<std::ptrdiff_t>(result.transferred),
                                       expected.begin() + static_cast<std::ptrdiff_t>(received)),
                            "corrupt bytes");
                    received += result.transferred;
                }
            }
            require(sent == expected.size() && received == expected.size(), "EOF lost buffered bytes");
            check(service.shutdown_until(soon()));
        }

        std::cout << "PASS: standalone stream, directional types, partial transfers, drain before EOF\n";
        {
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
            for (int i = 0; i < 2; ++i) {
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

        std::cout << "PASS: independent directions, half close by composition, closed reader\n";
        {
            PipeServiceOptions options;
            options.max_endpoints = 3;
            auto service = take(PipeService::create(options));
            auto pipe = take(service.create_pipe());
            for (int i = 0; i < 8; ++i) {
                auto rejected = service.create_pipe();
                require(!rejected && rejected.error().code == ErrorCode::ResourceExhausted,
                        "partial pair admitted");
            }

            auto other = take(PipeService::create());
            IoBuffer buffer(7);
            auto wrong = other.submit_read(pipe.reader, std::move(buffer), soon());
            require(!wrong && wrong.error().code == ErrorCode::InvalidArgument && buffer.size() == 7,
                    "wrong service consumed buffer");
            auto moved = std::move(pipe.reader);
            require(!pipe.reader && moved, "reader ownership move");
            auto pending = take(service.submit_read(moved, std::move(buffer), soon()));
            moved = PipeReader{};
            auto result = next(service);
            require(result.id == pending && result.status == IoStatus::Closed && result.buffer.size() == 7,
                    "reader destruction lost I/O");
            check(service.shutdown_until(soon()));
            check(other.shutdown_until(soon()));
        }

        std::cout << "PASS: atomic pair capacity, service identity, move with pending read\n";
        {
            auto service = take(PipeService::create());
            auto pipe = take(service.create_pipe());
            auto id = take(service.submit_read(pipe.reader, IoBuffer(4), soon()));
            std::stop_source stop;
            std::jthread stopper([&] {
                stop.request_stop();
            });
            std::array<IoCompletion, 1> output;
            auto cancelled_wait = service.wait_completions(output, soon(), stop.get_token());
            require(!cancelled_wait && cancelled_wait.error().code == ErrorCode::Cancelled,
                    "wait not cancelled");
            take(service.cancel(id));
            auto result = next(service);
            require(result.id == id && result.status == IoStatus::Cancelled, "read cancellation");
            take(service.submit_read(pipe.reader, std::move(result.buffer), Deadline::min()));
            require(next(service).status == IoStatus::TimedOut, "expired read");
            bool stalled = false;
            for (int i = 0; i < 128 && !stalled; ++i) {
                take(service.submit_write(pipe.writer, IoBuffer(65536),
                                          std::chrono::steady_clock::now() + 30ms));
                result = next(service);
                stalled = result.status == IoStatus::TimedOut;
                require(stalled || result.status == IoStatus::Completed, "backpressure result");
            }
            require(stalled, "unbounded transport buffer");
            check(service.shutdown_until(soon()));
        }

        std::cout << "PASS: wait cancellation, read cancellation/deadline, bounded backpressure\n";
        {
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
            require(!rejected && rejected.error().code == ErrorCode::ServiceClosed, "create after shutdown");
        }

        std::cout << "PASS: writer move assignment, shutdown drain and closed admission\n";
        {
#ifdef _WIN32
            DWORD before = 0;
            require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count");
#endif
            for (int i = 0; i < 50; ++i) {
                auto service = take(PipeService::create());
                auto pipe = take(service.create_pipe());
                take(service.submit_read(pipe.reader, IoBuffer(1), soon()));
                check(service.shutdown_until(soon()));
                require(next(service).status == IoStatus::Closed, "pending close");
            }
#ifdef _WIN32
            DWORD after = 0;
            require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before + 2,
                    "standalone handle leak");
#endif
        }

        std::cout << "PASS: standalone service/pipe churn and resource collection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
