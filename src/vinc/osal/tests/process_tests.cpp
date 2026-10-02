// Process contract tests using real child executables: arguments, environment,
// exit observation, termination, cancellation and resource ownership.

#include <vinc/osal/process.hpp>

#include <atomic>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace vinc::osal;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible_v<Process>);
static_assert(std::is_nothrow_move_constructible_v<Process>);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error("OSAL error " + std::to_string(static_cast<int>(result.error().code)) +
                                 ": " + result.error().native.message());
    }

    return std::move(*result);
}

void ok(Result<void> result) {
    if (!result) {
        throw std::runtime_error("OSAL error " + std::to_string(static_cast<int>(result.error().code)) +
                                 ": " + result.error().native.message());
    }
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string read_field(std::ifstream& input) {
    std::size_t size = 0;
    input >> size;
    require(input.get() == '\n' && size < 1024 * 1024, "invalid report length");
    std::string result(size, '\0');
    input.read(result.data(), static_cast<std::streamsize>(size));
    require(input.get() == '\n', "truncated report");
    return result;
}

struct Harness {
    ProcessService service = take(ProcessService::create());
    std::filesystem::path helper;
    std::vector<ProcessId> ids;

    explicit Harness(std::filesystem::path path) : helper(std::move(path)) {
    }

    ~Harness() {
        for (auto id : ids) {
            auto ignored = service.terminate(id);
            (void)ignored;
        }

        auto ignored = service.shutdown_until(std::chrono::steady_clock::now() + 10s);
        (void)ignored;
    }

    SpawnOptions options(std::vector<std::string> args) const {
        SpawnOptions result;
        result.executable = helper;
        result.arguments = std::move(args);
        result.stdout_mode = result.stderr_mode = StdioMode::Null;
        return result;
    }

    Process spawn_with(const SpawnOptions& options) {
        auto child = take(service.spawn(options));
        ids.push_back(child.id());
        return child;
    }

    Process spawn(std::vector<std::string> args) {
        return spawn_with(options(std::move(args)));
    }

    ExitStatus wait(Process& child) {
        return take(child.wait_until(std::chrono::steady_clock::now() + 10s));
    }
};

int tests(const std::filesystem::path& helper) {
    Harness test(helper);
    {
        auto child = test.spawn({"exit", "7"});
        const auto status = test.wait(child);
        require(status.kind == ExitKind::Code && status.value == 7, "exit code lost");
        require(take(child.try_wait()) == status, "exit result not cached");
        require(take(child.terminate()) == TerminateResult::AlreadyExited, "exited child termination");
#ifndef _WIN32
        int ignored;
        require(waitpid(static_cast<pid_t>(child.pid()), &ignored, WNOHANG) == -1 && errno == ECHILD,
                "zombie was not reaped");
#endif
        ok(test.service.forget(child.id()));
        require(!test.service.find(child.id()), "forgotten record retained");
        require(take(child.try_wait()) == status, "forget invalidated existing view");
    }

    std::cout << "PASS: exit, cached result, reaping, forget\n";
    {
        auto child = test.spawn({"sleep", "5000"});
        require(!take(child.try_wait()), "sleeping child exited");
        auto timeout = child.wait_until(std::chrono::steady_clock::now() + 10ms);
        require(!timeout && timeout.error().code == ErrorCode::Timeout, "wait timeout");
        std::stop_source cancel;
        std::jthread request([&] {
            std::this_thread::sleep_for(20ms);
            cancel.request_stop();
        });
        auto cancelled = child.wait_until(std::chrono::steady_clock::now() + 2s, cancel.get_token());
        require(!cancelled && cancelled.error().code == ErrorCode::Cancelled, "wait cancellation");
        require(!take(child.try_wait()), "cancellation killed child");
        auto busy = test.service.forget(child.id());
        require(!busy && busy.error().code == ErrorCode::Busy, "forgot live child");
        require(take(child.terminate()) == TerminateResult::Requested, "force terminate");
        test.wait(child);
    }

    std::cout << "PASS: timeout, cancellation, explicit termination\n";
    {
        auto child = test.spawn({"sleep", "5000"});
        std::latch ready(8);
        std::vector<std::optional<Result<ExitStatus>>> results(8);
        std::vector<std::jthread> waiters;
        for (std::size_t i = 0; i < results.size(); ++i) {
            waiters.emplace_back([&, i] {
                ready.count_down();
                results[i] = child.wait_until(std::chrono::steady_clock::now() + 5s);
            });
        }
        ready.wait();
        take(test.service.terminate(child.id()));
        waiters.clear();
        const auto status = test.wait(child);
        for (auto& result : results) {
            require(take(std::move(*result)) == status, "concurrent wait result mismatch");
        }
    }

    std::cout << "PASS: concurrent wait and central termination\n";
    {
        auto child = test.spawn({"sleep", "5000"});
        const auto id = child.id();
        Process moved = std::move(child);
        require(!child && moved.id() == id, "move ownership");
        moved = Process{};
        auto recovered = take(test.service.find(id));
        require(!take(recovered.try_wait()), "dropping view killed child");
        take(test.service.terminate(id));
        test.wait(recovered);
    }

    std::cout << "PASS: central control after dropping Process\n";
    {
        auto options = test.options({"exit", "0"});
        options.executable = "relative-program";
        auto invalid = test.service.spawn(options);
        require(!invalid && invalid.error().code == ErrorCode::InvalidArgument,
                "relative executable accepted");
        options.executable = helper.parent_path() / "vinc-does-not-exist";
        auto missing = test.service.spawn(options);
        require(!missing && missing.error().code == ErrorCode::NotFound, "missing executable");
        options = test.options({std::string("a\0b", 3)});
        require(!test.service.spawn(options), "embedded NUL accepted");
        options.arguments = {std::string("\xc0\x80", 2)};
        require(!test.service.spawn(options), "invalid UTF-8 accepted");
        options = test.options({"exit", "0"});
        options.environment = {{"bad=name", "value"}};
        require(!test.service.spawn(options), "invalid environment name accepted");
        options = test.options({"exit", "0"});
        options.working_directory = helper.parent_path() / "vinc-missing-directory";
        require(!test.service.spawn(options), "missing cwd accepted");
    }

    std::cout << "PASS: invalid inputs and missing executable\n";
#ifdef _WIN32
    {
        auto child = test.spawn({"exit", "259"});
        require(test.wait(child).value == 259, "exit code 259 confused with STILL_ACTIVE");
    }

    std::cout << "PASS: native exit code 259\n";
#endif
    {
        const auto directory =
            std::filesystem::current_path() /
            from_utf8("osal \xe6\xb8\xac\xe8\xa9\xa6 " +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        // All generated test files stay below the test's build working directory.
        const auto unicode_helper = directory / helper.filename();
        std::filesystem::copy_file(helper, unicode_helper);
        const auto report = directory / "report.txt";
        const std::vector<std::string> arguments{
            "", "a b", "a\"b", "trailing\\", "slashes\\\"quote", "\xe4\xb8\xad\xe6\x96\x87"};
        auto options = test.options({"report", utf8_path(report)});
        options.executable = unicode_helper;
        options.arguments.insert(options.arguments.end(), arguments.begin(), arguments.end());
        options.working_directory = directory;
        options.environment_mode = EnvironmentMode::Replace;
        options.environment = {{"VINC_OSAL_TEST_VALUE", arguments.back()},
                               {"VINC_OSAL_TEST_REMOVE", "remove me"},
                               {"VINC_OSAL_TEST_REMOVE", std::nullopt}};
        const auto original_directory = std::filesystem::current_path();
        auto child = test.spawn_with(options);
        require(test.wait(child).value == 0, "report helper failed");
        require(std::filesystem::current_path() == original_directory, "parent cwd changed");
        std::ifstream input(report, std::ios::binary);
        require(std::filesystem::equivalent(from_utf8(read_field(input)), directory), "child cwd mismatch");
        require(read_field(input) == arguments.back(), "Unicode environment mismatch");
        require(read_field(input) == "<missing>", "environment removal failed");
        for (const auto& argument : arguments) {
            require(read_field(input) == argument, "argument quoting/encoding mismatch");
        }
        input.close();
        std::filesystem::remove(report);
        std::filesystem::remove(unicode_helper);
        std::filesystem::remove(directory); // empty directory only; never recursive
    }

    std::cout << "PASS: Unicode executable, arguments, cwd, environment\n";
    {
        auto options = test.options({"null-stdio"});
        options.stdin_mode = options.stdout_mode = options.stderr_mode = StdioMode::Null;
        auto child = test.spawn_with(options);
        require(test.wait(child).value == 0, "null stdio");
    }

    std::cout << "PASS: null standard streams\n";
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE event = CreateEventW(&attributes, TRUE, FALSE, nullptr);
        require(event != nullptr, "create inheritable test event");
        auto child = test.spawn({"probe-resource", std::to_string(reinterpret_cast<std::uintptr_t>(event))});
        test.wait(child);
        const bool isolated = WaitForSingleObject(event, 0) == WAIT_TIMEOUT;
        CloseHandle(event);
        require(isolated, "unlisted Windows handle was inherited");
#else
        int descriptors[2];
        require(pipe(descriptors) == 0, "create inheritable pipe");
        fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
        auto child = test.spawn({"probe-resource", std::to_string(descriptors[1])});
        test.wait(child);
        char byte;
        const bool isolated = read(descriptors[0], &byte, 1) < 0 && errno == EAGAIN;
        close(descriptors[0]);
        close(descriptors[1]);
        require(isolated, "unlisted POSIX descriptor was inherited");
#endif
    }

    std::cout << "PASS: native resource inheritance isolation\n";
    {
        auto crash = test.spawn({"abort"});
        auto survivor = test.spawn({"sleep", "100", "23"});
        const auto crash_status = test.wait(crash);
        require(crash_status.kind == ExitKind::Signal || crash_status.value != 0,
                "abort appeared successful");
        require(test.wait(survivor).value == 23, "one child's failure affected another");
    }

    std::cout << "PASS: abnormal exit and process independence\n";
    {
        std::vector<std::optional<Result<Process>>> spawned(12);
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < spawned.size(); ++i) {
            threads.emplace_back([&, i] {
                spawned[i] = test.service.spawn(test.options({"exit", std::to_string(i)}));
            });
        }
        threads.clear();
        for (std::size_t i = 0; i < spawned.size(); ++i) {
            auto child = take(std::move(*spawned[i]));
            test.ids.push_back(child.id());
            require(test.wait(child).value == i, "concurrent spawn result mismatch");
            ok(test.service.forget(child.id()));
        }

        for (int i = 0; i < 40; ++i) {
            auto child = test.spawn({"exit", "0"});
            test.wait(child);
            ok(test.service.forget(child.id()));
        }
    }

    std::cout << "PASS: concurrent spawn and repeated collection\n";
    {
        auto service = take(ProcessService::create({1}));
        auto missing_options = test.options({"exit", "0"});
        missing_options.executable = helper.parent_path() / "vinc-missing-executable";
        require(!service.spawn(missing_options), "missing executable unexpectedly created");
        auto child = take(service.spawn(test.options({"exit", "0"})));
        take(child.wait_until(std::chrono::steady_clock::now() + 5s));
        auto full = service.spawn(test.options({"exit", "0"}));
        require(!full && full.error().code == ErrorCode::ResourceExhausted, "record bound not enforced");
        ok(service.forget(child.id()));
        auto next = take(service.spawn(test.options({"exit", "0"})));
        require(next.id() != child.id(), "ProcessId reused");
        take(next.wait_until(std::chrono::steady_clock::now() + 5s));
        ok(service.shutdown_until(std::chrono::steady_clock::now() + 5s));
    }

    std::cout << "PASS: bounded records and stable identity\n";
    {
        auto service = take(ProcessService::create());
        auto child = take(service.spawn(test.options({"sleep", "5000"})));
        auto timeout = service.shutdown_until(std::chrono::steady_clock::now() + 10ms);
        require(!timeout && timeout.error().code == ErrorCode::Timeout, "shutdown timeout");
        auto closed = service.spawn(test.options({"exit", "0"}));
        require(!closed && closed.error().code == ErrorCode::ServiceClosed, "spawn after shutdown");
        take(service.terminate(child.id()));
        take(child.wait_until(std::chrono::steady_clock::now() + 5s));
        ok(service.shutdown_until(std::chrono::steady_clock::now() + 5s));
    }

    std::cout << "PASS: bounded shutdown retains control\n";
    {
        Process surviving_view;
        {
            auto service = take(ProcessService::create());
            surviving_view = take(service.spawn(test.options({"sleep", "30", "19"})));
        }
        require(take(surviving_view.wait_until(std::chrono::steady_clock::now() + 5s)).value == 19,
                "service destruction killed child or lost reaping");
    }

    std::cout << "PASS: reaper lifetime after service facade destruction\n";
    require(!test.service.monitor_error(), "native event monitor failed");
    ok(test.service.shutdown_until(std::chrono::steady_clock::now() + 5s));
    return 0;
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected helper path");
        return tests(std::filesystem::absolute(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
