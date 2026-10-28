// Each closed-stdio/resource-limit scenario runs in a fresh, single-threaded
// fork child. No live OSAL service or FileLock is carried across fork.
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto child_timeout = 5s;
constexpr auto reap_interval = 5ms;
constexpr int inherited_input_timeout_ms = 5000;
constexpr std::uint64_t memory_bytes = 4096;
constexpr int stdio_count = 3;
constexpr std::size_t failure_message_capacity = 512;
constexpr int inherited_descriptor_count = 3;
constexpr int sparse_descriptor_minimum = 32;

void require(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
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

void close_stdio() {
    for (int fd = 0; fd < stdio_count; ++fd) {
        (void)::close(fd);
    }
}

void require_missing_stdio() {
    for (int fd = 0; fd < stdio_count; ++fd) {
        require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "internal fd occupied stdio");
    }
}

// Anonymous shared memory keeps diagnostics independent of the fd table being
// tested. The parent reads only after waitpid confirms that the child exited.
struct ChildFailure {
    char message[failure_message_capacity]{};
};

class ChildDiagnostics {
public:
    ChildDiagnostics() {
        void* mapping = ::mmap(nullptr, sizeof(ChildFailure), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        require(mapping != MAP_FAILED, "map child diagnostics");
        // mmap supplies page-aligned storage; placement new starts the lifetime
        // of this trivial record before fork gives both processes the mapping.
        failure_ = ::new (mapping) ChildFailure{};
    }

    ChildDiagnostics(const ChildDiagnostics&) = delete;
    ChildDiagnostics& operator=(const ChildDiagnostics&) = delete;

    ~ChildDiagnostics() {
        (void)::munmap(failure_, sizeof(ChildFailure));
    }

    void record(const char* message) noexcept {
        const auto length = ::strnlen(message, failure_message_capacity - 1);
        std::memcpy(failure_->message, message, length);
        failure_->message[length] = '\0';
    }

    void check(int exit, const char* stage) const {
        if (exit != 0) {
            const char* reason = "child exited abnormally or exceeded its deadline";
            if (failure_->message[0] != '\0') {
                reason = failure_->message;
            }
            throw std::runtime_error(std::string(stage) + ": " + reason);
        }
    }

private:
    ChildFailure* failure_ = nullptr;
};

// Keep launcher-owned descriptors intact. These extra descriptors are inherited
// across fork, including one above a gap, and are owned only by this fixture.
struct InheritedDescriptors {
    int descriptors[inherited_descriptor_count] = {-1, -1, -1};
    int sparse = -1;

    InheritedDescriptors() = default;
    InheritedDescriptors(const InheritedDescriptors&) = delete;
    InheritedDescriptors& operator=(const InheritedDescriptors&) = delete;

    void occupy() {
        for (int& fd : descriptors) {
            fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            require(fd >= stdio_count, "open inherited descriptor");
        }

        sparse = ::fcntl(descriptors[0], F_DUPFD_CLOEXEC, sparse_descriptor_minimum);
        require(sparse >= sparse_descriptor_minimum, "open sparse inherited descriptor");
    }

    ~InheritedDescriptors() {
        for (int fd : descriptors) {
            if (fd >= 0) {
                (void)::close(fd);
            }
        }

        if (sparse >= 0) {
            (void)::close(sparse);
        }
    }
};

struct Workspace {
    std::filesystem::path directory;
    std::filesystem::path lock;
    std::string memory_name = "fd_" + std::to_string(::getpid());

    Workspace() {
        auto pattern = (std::filesystem::current_path() / "osal-fd-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "private directory");
        directory = pattern;
        lock = directory / "resource.lock";
    }

    ~Workspace() {
        (void)SharedMemory::unlink_name(memory_name);
        std::error_code ignored;
        std::filesystem::remove(lock, ignored);
        std::filesystem::remove(directory, ignored);
    }
};

struct Child {
    Process process;

    ~Child() {
        (void)process.terminate();
        (void)process.wait_until(std::chrono::steady_clock::now() + child_timeout);
    }
};

void isolated_resources(const std::filesystem::path& executable) {
    Workspace workspace;
    close_stdio();
    auto service = take(ProcessService::create());
    require_missing_stdio();

    auto lock = take(FileLock::create(workspace.lock));
    auto opened_lock = take(FileLock::open(workspace.lock));
    auto guard = take(lock.try_lock());
    auto conflict = opened_lock.try_lock();
    require(!conflict && conflict.error().is(ErrorCode::WouldBlock), "lock exclusion");
    require_missing_stdio();

    auto memory = take(SharedMemory::create(workspace.memory_name, memory_bytes));
    auto opened_memory = take(SharedMemory::open(workspace.memory_name, SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(memory_bytes, SharedMemoryAccess::ReadWrite));
    auto other_mapping = take(opened_memory.map(memory_bytes, SharedMemoryAccess::ReadWrite));
    take(mapping.writable_bytes())[0] = std::byte{0x51};
    require(other_mapping.bytes()[0] == std::byte{0x51}, "mapping after relocation");
    require_missing_stdio();

    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--null-stdio"};
    options.stdin_mode = StdioMode::Inherit;
    Child child{take(service.spawn(options))};
    // Linux pidfd_open runs during spawn, after monitor creation.
    require_missing_stdio();
    auto exit = take(child.process.wait_until(std::chrono::steady_clock::now() + child_timeout));
    require(exit.kind == ExitKind::Code && exit.value == 0, "missing stdio was not /dev/null");
    take(service.shutdown_until(std::chrono::steady_clock::now() + child_timeout));
}

struct DescriptorLimit {
    rlimit original{};
    int filler = -1;
    bool restricted = false;

    DescriptorLimit(const DescriptorLimit&) = delete;
    DescriptorLimit& operator=(const DescriptorLimit&) = delete;

    DescriptorLimit() {
        require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "read descriptor limit");
        // F_DUPFD_CLOEXEC selects the lowest free fd >= 3. All smaller
        // non-stdio numbers are therefore already occupied, possibly by CTest.
        const int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        require(source >= 0, "limit fixture source");
        filler = ::fcntl(source, F_DUPFD_CLOEXEC, stdio_count);
        (void)::close(source);
        require(filler >= stdio_count, "duplicate limit fixture source");
        const rlimit limit{static_cast<rlim_t>(filler) + 1, original.rlim_max};
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
            (void)::close(filler);
            filler = -1;
            throw std::runtime_error("set descriptor limit");
        }

        restricted = true;
    }

    ~DescriptorLimit() {
        if (restricted) {
            (void)::setrlimit(RLIMIT_NOFILE, &original);
        }

        if (filler >= 0) {
            (void)::close(filler);
        }
    }

    void restore() {
        require(::setrlimit(RLIMIT_NOFILE, &original) == 0, "restore descriptor limit");
        restricted = false;
    }

    void verify_exhausted() const {
        require_missing_stdio();
        const int probe = ::fcntl(filler, F_DUPFD_CLOEXEC, stdio_count);
        const int failure = errno;
        if (probe >= 0) {
            (void)::close(probe);
        }

        require(probe == -1 && failure == EMFILE, "fixture did not exhaust non-stdio descriptors");
    }
};

void relocation_failure() {
    Workspace workspace;
    close_stdio();
    {
        DescriptorLimit limit;
        limit.verify_exhausted();
        auto lock = FileLock::create(workspace.lock);
        require(!lock && lock.error().is(ErrorCode::ResourceExhausted), "lock relocation failure");
        require(lock.error().is_operation(ErrorOperation::CreateFileLock), "lock operation");
        require(lock.error().native.value() == EMFILE, "lock native error");
        require_missing_stdio();
        require(!std::filesystem::exists(workspace.lock), "failed lock left a new name");

        auto memory = SharedMemory::create(workspace.memory_name, memory_bytes);
        require(!memory && memory.error().is(ErrorCode::ResourceExhausted), "memory relocation failure");
        require(memory.error().is_operation(ErrorOperation::CreateSharedMemory), "memory operation");
        require(memory.error().native.value() == EMFILE, "memory native error");
        require_missing_stdio();

        auto service = ProcessService::create();
        require(!service && service.error().is(ErrorCode::ResourceExhausted), "monitor relocation failure");
        require_missing_stdio();
        limit.restore();
    }

    // Retry with the same names: failed creation must have rolled them back.
    auto lock = take(FileLock::create(workspace.lock));
    auto memory = take(SharedMemory::create(workspace.memory_name, memory_bytes));
    lock.close();
    take(memory.close());
    {
        DescriptorLimit limit;
        limit.verify_exhausted();
        auto opened_lock = FileLock::open(workspace.lock);
        require(!opened_lock && opened_lock.error().is(ErrorCode::ResourceExhausted), "lock open failure");
        require(opened_lock.error().is_operation(ErrorOperation::OpenFileLock), "lock open operation");
        require(opened_lock.error().native.value() == EMFILE, "lock open native error");
        auto opened_memory = SharedMemory::open(workspace.memory_name, SharedMemoryAccess::ReadOnly);
        require(!opened_memory && opened_memory.error().is(ErrorCode::ResourceExhausted), "memory open failure");
        require(opened_memory.error().is_operation(ErrorOperation::OpenSharedMemory), "memory open operation");
        require(opened_memory.error().native.value() == EMFILE, "memory open native error");
        require_missing_stdio();
        limit.restore();
    }
    // Open failures must not unlink someone else's existing resource.
    auto opened_lock = take(FileLock::open(workspace.lock));
    auto opened_memory = take(SharedMemory::open(workspace.memory_name, SharedMemoryAccess::ReadOnly));
}

int reap(pid_t child) {
    const auto deadline = std::chrono::steady_clock::now() + child_timeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (waited < 0 && errno != EINTR) {
            return 1;
        }

        std::this_thread::sleep_for(reap_interval);
    }

    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return 1;
}

void owner_exit(const std::filesystem::path& executable) {
    Workspace workspace;
    ChildDiagnostics diagnostics;
    int control[2];
    require(::pipe(control) == 0, "owner control pipe");
    const auto owner = ::fork();
    require(owner >= 0, "fork owner");
    if (owner == 0) {
        int result = 1;
        try {
            (void)::close(control[1]);
            require(::dup2(control[0], STDIN_FILENO) == STDIN_FILENO, "owner stdin");
            (void)::close(control[0]);
            (void)::close(STDOUT_FILENO);
            auto lock = take(FileLock::create(workspace.lock));
            auto guard = take(lock.try_lock());
            auto service = take(ProcessService::create());
            SpawnOptions options;
            options.executable = executable;
            options.arguments = {"--hold-input"};
            options.stdin_mode = StdioMode::Inherit;
            auto child = take(service.spawn(options));
            // Deliberately skip destructors: only OS cleanup may release the lock.
            // The supervisor's pipe keeps the grandchild alive until after the probe.
            ::_exit(0);
        } catch (const std::exception& error) {
            diagnostics.record(error.what());
            result = 1;
        }
        ::_exit(result);
    }

    (void)::close(control[0]);
    const auto exit = reap(owner);
    auto lock = FileLock::open(workspace.lock);
    bool released = false;
    if (lock) {
        auto guard = lock->try_lock();
        released = guard.has_value();
    }
    // EOF releases the grandchild even if the lock assertion fails. Its native
    // poll timeout is an additional bound if the supervisor itself is terminated.
    (void)::close(control[1]);
    diagnostics.check(exit, "owner death");
    require(exit == 0 && released, "dead owner's lock retained by a child");
}

void run_isolated_case(const std::filesystem::path& executable, bool failure_case, bool inherit_fds) {
    InheritedDescriptors inherited;
    if (inherit_fds) {
        inherited.occupy();
    }
    ChildDiagnostics diagnostics;
    const auto child = ::fork();
    require(child >= 0, "fork isolated test");
    if (child == 0) {
        try {
            if (failure_case) {
                relocation_failure();
            } else {
                isolated_resources(executable);
            }
            ::_exit(0);
        } catch (const std::exception& error) {
            diagnostics.record(error.what());
            ::_exit(1);
        }
    }
    const char* stage = failure_case ? "relocation rollback" : "stdio isolation";
    if (inherit_fds) {
        stage = failure_case ? "relocation rollback with inherited fds" : "stdio isolation with inherited fds";
    }
    diagnostics.check(reap(child), stage);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--hold-input") {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        (void)::poll(&input, 1, inherited_input_timeout_ms);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--null-stdio") {
        struct stat expected{};
        if (::stat("/dev/null", &expected) != 0) {
            return 1;
        }
        for (int fd = 0; fd < stdio_count; ++fd) {
            struct stat actual{};
            if (::fstat(fd, &actual) != 0 || !S_ISCHR(actual.st_mode) || actual.st_rdev != expected.st_rdev) {
                return 1;
            }
        }
        return 0;
    }

    try {
        const auto executable = std::filesystem::absolute(argv[0]);
        for (bool inherit_fds : {false, true}) {
            for (bool failure_case : {false, true}) {
                run_isolated_case(executable, failure_case, inherit_fds);
            }
        }
        owner_exit(executable);
        std::cout << "PASS: descriptor isolation, relocation rollback and owner death\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
