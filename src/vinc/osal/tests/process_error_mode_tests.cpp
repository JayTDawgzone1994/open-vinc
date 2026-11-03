// Real Windows loader failures must return without a test-wide suppression flag.
#include <vinc/osal/path.hpp>
#include <vinc/osal/process_pipe.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>

using namespace vinc::osal;
using namespace std::chrono_literals;

namespace {
constexpr auto child_timeout = 5s;
constexpr int failure_repetitions = 4;

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

void take(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("vinc-error-mode-" + std::to_string(GetCurrentProcessId()));

    TemporaryDirectory() {
        require(std::filesystem::create_directory(path), "temporary directory already exists");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// This executable is isolated by CTest. Process-wide changes are test inputs,
// never the mechanism that prevents dialogs in the production spawn under test.
struct TestModes {
    DWORD process = GetErrorMode();
    DWORD thread = GetThreadErrorMode();

    ~TestModes() {
        SetErrorMode(process);
        (void)SetThreadErrorMode(thread, nullptr);
    }
};

void check_failure(const vinc::Error& error, bool missing) {
    require(error.domain == vinc::ErrorDomain::Osal, "wrong error domain");
    require(error.operation == static_cast<std::int32_t>(ErrorOperation::Spawn), "wrong operation");
    require(error.native.source == vinc::NativeErrorSource::Win32, "native source lost");
    if (missing) {
        require(error.is(ErrorCode::NotFound), "missing file classification lost");
        require(error.native.value() == ERROR_FILE_NOT_FOUND, "missing file diagnostic lost");
    } else {
        require(error.native.value() == ERROR_BAD_EXE_FORMAT ||
                    error.native.value() == ERROR_EXE_MACHINE_TYPE_MISMATCH,
                "bad image diagnostic lost");
    }
}

void wait_and_forget(ProcessService& service, Process& child) {
    const auto result = child.wait_until(std::chrono::steady_clock::now() + child_timeout);
    if (!result) {
        (void)child.terminate();
        (void)child.wait_until(std::chrono::steady_clock::now() + child_timeout);
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    take(service.forget(child.id()));
    require(result->kind == ExitKind::Code && result->value == 0, "child inherited wrong error mode");
}

void exercise(const std::filesystem::path& self, const std::filesystem::path& malformed,
              DWORD process_mode, DWORD thread_mode) {
    TestModes restore;
    SetErrorMode(process_mode);
    require(SetThreadErrorMode(thread_mode, nullptr) != FALSE, "cannot set fixture thread mode");

    std::atomic<bool> other_thread_unchanged{true};
    DWORD observer_initial_mode = 0;
    DWORD observer_failure_mode = 0;
    std::latch observer_ready{1};
    std::jthread observer([&](std::stop_token stop) {
        // Thread startup may establish its own mode; compare with its actual
        // baseline rather than assuming it equals the process-wide setting.
        observer_initial_mode = GetThreadErrorMode();
        observer_ready.count_down();
        while (!stop.stop_requested()) {
            // Native CreateProcessW itself can temporarily change process mode,
            // even without OSAL. Check that mode at API boundaries instead.
            const DWORD current_thread_mode = GetThreadErrorMode();
            if (current_thread_mode != observer_initial_mode) {
                observer_failure_mode = current_thread_mode;
                other_thread_unchanged.store(false);
            }
            std::this_thread::yield();
        }
    });
    observer_ready.wait();

    // A single record catches failed-launch admission leaks before the valid retry.
    auto service = take(ProcessService::create(ServiceOptions{1}));
    auto pipes = take(PipeService::create());
    SpawnOptions options;
    options.stdin_mode = StdioMode::Null;
    options.stdout_mode = StdioMode::Null;
    options.stderr_mode = StdioMode::Null;
    for (bool capture : {false, true}) {
        options.stdout_mode = capture ? StdioMode::Capture : StdioMode::Null;
        options.stderr_mode = capture ? StdioMode::Capture : StdioMode::Null;
        for (bool missing : {false, true}) {
            options.executable = missing ? malformed.parent_path() / "missing.exe" : malformed;
            for (int attempt = 0; attempt < failure_repetitions; ++attempt) {
                if (capture) {
                    auto result = spawn_with_output(service, pipes, options);
                    if (result) {
                        (void)result->process.terminate();
                        wait_and_forget(service, result->process);
                        throw std::runtime_error("bad executable unexpectedly launched");
                    }

                    check_failure(result.error(), missing);
                } else {
                    auto result = service.spawn(options);
                    if (result) {
                        (void)result->terminate();
                        wait_and_forget(service, *result);
                        throw std::runtime_error("bad executable unexpectedly launched");
                    }

                    check_failure(result.error(), missing);
                }
                require(GetThreadErrorMode() == thread_mode, "failed spawn changed thread mode");
                require(GetErrorMode() == process_mode, "failed spawn changed process mode");
            }
        }

        options.executable = self;
        options.arguments = {"--check-mode", std::to_string(process_mode)};
        if (capture) {
            auto child = take(spawn_with_output(service, pipes, options));
            wait_and_forget(service, child.process);
        } else {
            auto child = take(service.spawn(options));
            wait_and_forget(service, child);
        }
        options.arguments.clear();
        require(GetThreadErrorMode() == thread_mode, "successful spawn changed thread mode");
        require(GetErrorMode() == process_mode, "successful spawn changed process mode");
    }

    take(pipes.shutdown_until(std::chrono::steady_clock::now() + child_timeout));
    take(service.shutdown_until(std::chrono::steady_clock::now() + child_timeout));
    observer.request_stop();
    observer.join();
    if (!other_thread_unchanged.load()) {
        std::cerr << "observer modes: initial=" << observer_initial_mode
                  << " later=" << observer_failure_mode << '\n';
    }
    require(other_thread_unchanged.load(), "another thread's mode changed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--check-mode") {
            const auto expected = static_cast<DWORD>(std::stoul(argv[2]));
            // The child inherits the process mode, not the launching thread's
            // override. A fresh primary thread reports no thread override.
            return GetErrorMode() == expected && GetThreadErrorMode() == 0 ? 0 : 1;
        }

        TemporaryDirectory directory;
        const auto malformed = directory.path / "broken.exe";
        {
            std::ofstream file(malformed, std::ios::binary);
            file << "not an executable image\n";
            file.close();
            require(file.good(), "cannot write bad image fixture");
        }
        const auto self = take(executable_path());
        exercise(self, malformed, 0, 0);
        exercise(self, malformed, 0, SEM_NOOPENFILEERRORBOX);
        exercise(self, malformed, SEM_NOOPENFILEERRORBOX, SEM_NOGPFAULTERRORBOX);
        std::cout << "PASS: loader failures, restoration, isolation, inheritance and retry\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
