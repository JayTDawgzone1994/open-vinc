// Native contract probes run in child processes: a SIGPIPE regression must fail
// a case without killing the supervisor. No sleeps establish close/write order.
#include <vinc/osal/process_pipe.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto operation_timeout = 5s;
constexpr auto child_timeout = 15s;
constexpr std::uint32_t endpoint_slot = 42;
constexpr int broken_write_attempts = 3;
constexpr std::byte payload{0x5a};

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
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
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

IoCompletion next(PipeService& service) {
    std::array<IoCompletion, 1> output;
    require(take(service.wait_completions(output, soon())) == 1, "missing completion");
    return std::move(output[0]);
}

void close_barrier(PipeService& service) {
    // The worker reclaims all closing endpoints before retrying the marker read.
    // Observing its EOF therefore proves earlier close requests were processed.
    auto marker = take(service.create_pipe());
    marker.writer.close();
    take(service.submit_read(marker.reader, IoBuffer(1), soon()));
    require(next(service).status == IoStatus::EndOfStream, "close barrier did not reach EOF");
}

void broken_writes(PipeService& service, PipeWriter& writer) {
    IoBuffer buffer(1);
    buffer.bytes()[0] = payload;
    const auto* original = buffer.bytes().data();
    for (int attempt = 0; attempt < broken_write_attempts; ++attempt) {
        const auto id = take(service.submit_write(writer, std::move(buffer), soon()));
        auto result = next(service);
        require(result.id == id && result.pipe == writer.id() && result.kind == IoKind::Write &&
                    result.status == IoStatus::PeerClosed && result.transferred == 0 && !result.error &&
                    result.buffer.size() == 1 && result.buffer.bytes().data() == original &&
                    result.buffer.bytes()[0] == payload,
                "broken write lost status, identity or buffer");
        buffer = std::move(result.buffer);
        std::array<IoCompletion, 1> extra;
        auto waited = service.wait_completions(extra, vinc::Deadline::min());
        require(!waited && waited.error().is(ErrorCode::Timeout), "duplicate completion");
    }
}

#ifndef _WIN32
std::atomic<unsigned> signal_calls{0};
static_assert(std::atomic<unsigned>::is_always_lock_free);

void pipe_signal_handler(int) {
    signal_calls.fetch_add(1, std::memory_order_relaxed);
}

bool same_mask(const sigset_t& first, const sigset_t& second) {
    for (int signal = 1; signal < NSIG; ++signal) {
        if (sigismember(&first, signal) != sigismember(&second, signal)) {
            return false;
        }
    }
    return true;
}

struct SignalPolicy {
    struct sigaction expected_action {};
    sigset_t expected_mask{};
    bool custom;

    explicit SignalPolicy(bool use_custom) : custom(use_custom) {
        // This policy belongs only to the isolated probe process, before OSAL
        // creates any threads. SIGUSR1 detects accidental whole-mask replacement.
        struct sigaction action {};
        action.sa_handler = custom ? pipe_signal_handler : SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGUSR2);
        action.sa_flags = SA_RESTART;
        require(sigaction(SIGPIPE, &action, nullptr) == 0, "set probe handler");
        sigset_t mask;
        require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0, "read caller mask");
        sigdelset(&mask, SIGPIPE);
        sigaddset(&mask, SIGUSR1);
        require(pthread_sigmask(SIG_SETMASK, &mask, nullptr) == 0, "set caller mask");
        require(sigaction(SIGPIPE, nullptr, &expected_action) == 0, "read probe handler");
        require(pthread_sigmask(SIG_SETMASK, nullptr, &expected_mask) == 0, "snapshot caller mask");
    }

    void verify() const {
        struct sigaction action {};
        sigset_t mask;
        require(sigaction(SIGPIPE, nullptr, &action) == 0, "read final handler");
        require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0, "read final caller mask");
        require(action.sa_handler == expected_action.sa_handler &&
                    action.sa_flags == expected_action.sa_flags &&
                    same_mask(action.sa_mask, expected_action.sa_mask) && same_mask(mask, expected_mask),
                "OSAL changed host signal policy");
        require(signal_calls.load(std::memory_order_relaxed) == 0, "OSAL write invoked host handler");
    }

    void verify_unrelated_pipe() const {
        if (custom) {
            // An unrelated pipe still invokes the host's handler after service
            // teardown. This catches process-wide SIG_IGN or caller mask changes.
            int descriptors[2];
            require(::pipe(descriptors) == 0, "create unrelated pipe");
            ::close(descriptors[0]);
            const char byte = 'x';
            const auto written = ::write(descriptors[1], &byte, 1);
            const int error = errno;
            ::close(descriptors[1]);
            require(written == -1 && error == EPIPE && signal_calls.load(std::memory_order_relaxed) == 1,
                    "unrelated pipe lost native SIGPIPE behavior");
        }
    }
};
#endif

int writer_probe(bool inherited, bool custom) {
#ifndef _WIN32
    const SignalPolicy signals(custom);
#else
    (void)custom;
#endif
    auto service = take(PipeService::create());
#ifndef _WIN32
    signals.verify();
#endif
    if (inherited) {
        auto endpoints = take(take_inherited_pipes(service));
        require(endpoints.size() == 1 && endpoints[0].slot == endpoint_slot &&
                    std::holds_alternative<PipeWriter>(endpoints[0].endpoint), "inherited writer missing");
        broken_writes(service, std::get<PipeWriter>(endpoints[0].endpoint));
    } else {
        auto pipe = take(service.create_pipe());
        pipe.reader.close();
        close_barrier(service);
        broken_writes(service, pipe.writer);
    }
#ifndef _WIN32
    signals.verify();
#endif
    check(service.shutdown_until(soon()));
#ifndef _WIN32
    signals.verify();
    signals.verify_unrelated_pipe();
#endif
    return 0;
}

int stdio_probe(bool standard_error, bool ignore_signal) {
#ifndef _WIN32
    struct sigaction action {};
    sigset_t mask;
    require(sigaction(SIGPIPE, nullptr, &action) == 0 && action.sa_handler == SIG_DFL,
            "spawn did not restore default SIGPIPE");
    require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 && sigismember(&mask, SIGPIPE) == 0,
            "spawn left SIGPIPE blocked");
    if (ignore_signal) {
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        require(sigaction(SIGPIPE, &action, nullptr) == 0, "set child SIGPIPE ignore policy");
    }
#else
    (void)ignore_signal;
#endif
    auto service = take(PipeService::create());
    auto endpoints = take(take_inherited_pipes(service));
    require(endpoints.size() == 1 && endpoints[0].slot == endpoint_slot &&
                std::holds_alternative<PipeReader>(endpoints[0].endpoint), "control reader missing");
    auto& reader = std::get<PipeReader>(endpoints[0].endpoint);
    take(service.submit_read(reader, IoBuffer(1), soon()));
    auto token = next(service);
    require(token.status == IoStatus::Completed && token.transferred == 1 &&
                token.buffer.bytes()[0] == payload, "close handshake failed");
    check(service.shutdown_until(soon()));

    // The parent has attached then closed the capture reader. Only this raw
    // standard-stream write may now terminate the child with SIGPIPE.
    const char byte = 'x';
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(standard_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD written = 0;
    const BOOL success = WriteFile(handle, &byte, 1, &written, nullptr);
    const DWORD error = GetLastError();
    require(!success && (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
                        error == ERROR_PIPE_NOT_CONNECTED), "raw stdio did not report broken pipe");
#else
    const auto written = ::write(standard_error ? STDERR_FILENO : STDOUT_FILENO, &byte, 1);
    const int error = errno;
    require(ignore_signal && written == -1 && error == EPIPE, "raw stdio lost native broken-pipe policy");
#endif
    return 0;
}

// Own every spawned child before asserting, so failed assertions/timeouts also
// terminate and reap it. CTest adds an outer bound for a stuck native operation.
struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes = take(PipeService::create());
    Process child;

    ~Harness() {
        if (child) {
            const auto terminated = child.terminate();
            (void)terminated;
            const auto waited = child.wait_until(soon());
            (void)waited;
        }
        const auto io = pipes.shutdown_until(soon());
        (void)io;
        const auto stopped = processes.shutdown_until(soon());
        (void)stopped;
    }
};

void run_case(const std::filesystem::path& executable, const std::string& mode) {
    Harness test;
    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"probe", mode};
    options.stdout_mode = StdioMode::Null;
    options.stderr_mode = StdioMode::Null;
    const bool inherited = mode.starts_with("inherited-");
    const bool stdio = mode.starts_with("stdio-");
    if (inherited) {
        auto pipe = take(test.pipes.create_pipe());
        pipe.reader.close();
        close_barrier(test.pipes);
        const std::array<PipeBinding, 1> bindings{{{endpoint_slot, &pipe.writer}}};
        test.child = take(spawn_with_pipes(test.processes, test.pipes, options, bindings));
    } else if (stdio) {
        const bool standard_error = mode.find("stderr") != std::string::npos;
        if (standard_error) {
            options.stderr_mode = StdioMode::Capture;
        } else {
            options.stdout_mode = StdioMode::Capture;
        }
        auto control = take(test.pipes.create_pipe());
        const std::array<PipeBinding, 1> bindings{{{endpoint_slot, &control.reader}}};
        auto captured = take(spawn_with_output(test.processes, test.pipes, options, bindings));
        test.child = std::move(captured.process);
        auto& reader = standard_error ? *captured.stderr_reader : *captured.stdout_reader;
        // Attach the reader before closing it, catching accidental propagation
        // of the reader's macOS flag to the child's distinct write end.
        const auto pending = take(test.pipes.submit_read(reader, IoBuffer(1), soon()));
        take(test.pipes.cancel(pending));
        require(next(test.pipes).status == IoStatus::Cancelled, "capture read cancel failed");
        reader.close();
        close_barrier(test.pipes);
        IoBuffer token(1);
        token.bytes()[0] = payload;
        take(test.pipes.submit_write(control.writer, std::move(token), soon()));
        auto sent = next(test.pipes);
        require(sent.status == IoStatus::Completed && sent.transferred == 1, "release child handshake");
    } else {
        test.child = take(test.processes.spawn(options));
    }

    const auto status = take(test.child.wait_until(std::chrono::steady_clock::now() + child_timeout));
#ifndef _WIN32
    if (stdio && mode.ends_with("default")) {
        require(status.kind == ExitKind::Signal && status.value == SIGPIPE, "raw stdio did not exit by SIGPIPE");
    } else
#endif
    {
        require(status.kind == ExitKind::Code && status.value == 0, "contract probe failed or was killed");
    }
    std::cout << "PASS: " << mode << '\n';
}
}

int main(int argc, char** argv) {
    const bool probe = argc == 3 && std::string_view(argv[1]) == "probe";
    try {
        require(argc == 3, "expected run/probe and case name");
        const std::string mode = argv[2];
        if (probe) {
            if (mode.starts_with("stdio-")) {
                return stdio_probe(mode.find("stderr") != std::string::npos, mode.ends_with("ignore"));
            }
            return writer_probe(mode.starts_with("inherited-"), mode.ends_with("custom"));
        }
        require(std::string_view(argv[1]) == "run", "unknown command");
        run_case(std::filesystem::absolute(argv[0]), mode);
        return 0;
    } catch (const std::exception& error) {
        // Never print into a closed captured stderr: that could disguise an
        // assertion failure as the SIGPIPE termination expected by the parent.
        if (!probe) {
            std::cerr << "FAIL: " << argv[argc - 1] << ": " << error.what() << '\n';
        }
        return 1;
    }
}
