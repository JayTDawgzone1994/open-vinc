// Run each destructive descriptor-table scenario in a fresh process. The
// supervisor never constructs OSAL services before fork and owns child cleanup.
#include <vinc/osal/detail/resource_coordinator.hpp>
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/notification.hpp>
#include <vinc/osal/pipe.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace vinc;
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto operation_timeout = 3s;
constexpr auto contention_probe = 40ms;
constexpr auto supervisor_timeout = 30s;
constexpr auto supervisor_poll = 5ms;
constexpr int stream_count = 3;
constexpr int fixture_descriptor_floor = 96;
constexpr int exhausted_descriptor_limit = 192;
constexpr std::uint64_t mapping_bytes = 4096;
constexpr std::uint32_t first_pipe_slot = 1000;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}
Deadline deadline() {
    return std::chrono::steady_clock::now() + operation_timeout;
}
void close_stdio() {
    for (int stream = 0; stream < stream_count; ++stream) {
        (void)::close(stream);
    }
}
bool missing_stdio() {
    for (int stream = 0; stream < stream_count; ++stream) {
        if (::fcntl(stream, F_GETFD) >= 0 || errno != EBADF) {
            return false;
        }
    }
    return true;
}

// The real coordinator owns exclusion; this borrowed table audits boundaries
// and can replace one parent stream exactly after snapshot ownership releases.
struct Audit {
    vinc::osal::detail::ResourceCoordinator owner;
    vinc::osal::detail::ResourceCoordination base = owner.services();
    std::atomic<unsigned> acquisitions{0};
    bool check_missing = false;
    bool exposed_stdio = false;
    int replace_stream = -1;
    int replacement = -1;
    bool replacement_failed = false;

    Audit() {
        auto services = base;
        services.context = this;
        services.acquire = acquire;
        services.release = release;
        take(vinc::osal::detail::bind_resource_coordination(services));
    }
    static Result<void> acquire(void* context, bool attempt, Deadline end, const std::stop_token& stop) {
        auto& audit = *static_cast<Audit*>(context);
        auto result = audit.base.acquire(audit.base.context, attempt, end, stop);
        if (result) {
            ++audit.acquisitions;
        }
        return result;
    }
    static void release(void* context) noexcept {
        auto& audit = *static_cast<Audit*>(context);
        if (audit.check_missing && !missing_stdio()) {
            audit.exposed_stdio = true;
        }
        if (audit.replace_stream >= 0) {
            audit.replacement_failed = ::dup2(audit.replacement, audit.replace_stream) < 0;
            audit.replace_stream = -1;
        }
        audit.base.release(audit.base.context);
    }
};

struct TempFile {
    int descriptor = -1;
    TempFile() {
        char name[] = "/tmp/ov-fd-spawn-XXXXXX";
        const int original = ::mkstemp(name);
        require(original >= 0, "create fixture file");
        (void)::unlink(name);
        descriptor = ::fcntl(original, F_DUPFD_CLOEXEC, fixture_descriptor_floor);
        (void)::close(original);
        require(descriptor >= fixture_descriptor_floor, "isolate fixture file");
    }
    ~TempFile() {
        (void)::close(descriptor);
    }
    TempFile(const TempFile&) = delete;
};

void expect_exit(Process& process) {
    auto result = process.wait_until(deadline());
    if (!result) {
        (void)process.terminate();
        (void)process.wait_until(deadline());
        take(std::move(result));
    }
    require(result->kind == ExitKind::Code && result->value == 0, "child identity/exit mismatch");
}
SpawnOptions probe(const std::filesystem::path& executable, int inherited_mask) {
    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"child", std::to_string(inherited_mask)};
    options.stdin_mode = StdioMode::Inherit;
    options.stdout_mode = StdioMode::Inherit;
    options.stderr_mode = StdioMode::Inherit;
    return options;
}
int child_probe(int inherited_mask) {
    for (int stream = 0; stream < stream_count; ++stream) {
        struct stat info{};
        if (::fstat(stream, &info) != 0 || (::fcntl(stream, F_GETFD) & FD_CLOEXEC) != 0) {
            return 2;
        }
        if (!(inherited_mask & (1 << stream))) {
            struct stat null_info{};
            if (::stat("/dev/null", &null_info) != 0 || !S_ISCHR(info.st_mode) ||
                info.st_rdev != null_info.st_rdev) {
                return 3;
            }
        } else if (stream == STDIN_FILENO) {
            char value = 0;
            if (::read(stream, &value, 1) != 1 || value != 'I') {
                return 4;
            }
        } else {
            const char value = static_cast<char>('0' + stream);
            if (::write(stream, &value, 1) != 1) {
                return 5;
            }
        }
    }
    return 0;
}

void matrix(const std::filesystem::path& executable) {
    Audit audit;
    std::array<TempFile, stream_count> files;
    auto service = take(ProcessService::create());
    constexpr int stream_masks = 1 << stream_count;
    for (int present = 0; present < stream_masks; ++present) {
        for (int cloexec = 0; cloexec < stream_masks; ++cloexec) {
            for (int inherit = 0; inherit < stream_masks; ++inherit) {
                close_stdio();
                for (int stream = 0; stream < stream_count; ++stream) {
                    const int source = files[stream].descriptor;
                    require(::ftruncate(source, 0) == 0, "reset fixture data");
                    require(::lseek(source, 0, SEEK_SET) == 0, "reset fixture offset");
                    if (stream == STDIN_FILENO) {
                        require(::write(source, "I", 1) == 1, "prepare input");
                        require(::lseek(source, 0, SEEK_SET) == 0, "rewind input");
                    }
                    if (present & (1 << stream)) {
                        require(::dup2(source, stream) == stream, "fixture stdio");
                        require(::fcntl(stream, F_SETFD, (cloexec & (1 << stream)) ? FD_CLOEXEC : 0) == 0,
                                "fixture CLOEXEC");
                    }
                }
                auto options = probe(executable, present & inherit);
                options.stdin_mode = (inherit & 1) ? StdioMode::Inherit : StdioMode::Null;
                options.stdout_mode = (inherit & 2) ? StdioMode::Inherit : StdioMode::Null;
                options.stderr_mode = (inherit & 4) ? StdioMode::Inherit : StdioMode::Null;
                auto child = take(service.spawn(options));
                expect_exit(child);
                take(service.forget(child.id()));
                for (int stream = 0; stream < stream_count; ++stream) {
                    const int flags = ::fcntl(stream, F_GETFD);
                    require((present & (1 << stream)) ? flags == ((cloexec & (1 << stream)) ? FD_CLOEXEC : 0)
                                                     : flags == -1 && errno == EBADF,
                            "spawn changed parent stdio flags/presence");
                    if (stream != STDIN_FILENO) {
                        char value = 0;
                        const auto count = ::pread(files[stream].descriptor, &value, 1, 0);
                        const bool delivered = (present & inherit & (1 << stream)) != 0;
                        require(delivered ? count == 1 && value == '0' + stream : count == 0,
                                "child output delivered to wrong resource");
                    }
                }
            }
        }
    }
    take(service.shutdown_until(deadline()));
}

void reuse(const std::filesystem::path& executable) {
    Audit audit;
    TempFile original;
    TempFile replacement;
    auto service = take(ProcessService::create());
    close_stdio();
    require(::dup2(original.descriptor, STDOUT_FILENO) == STDOUT_FILENO, "install original output");
    audit.replace_stream = STDOUT_FILENO;
    audit.replacement = replacement.descriptor;
    auto child = take(service.spawn(probe(executable, 2)));
    expect_exit(child);
    require(!audit.replacement_failed && audit.replace_stream == -1, "replacement hook not executed");
    char value = 0;
    require(::pread(original.descriptor, &value, 1, 0) == 1 && value == '1', "snapshot lost resource identity");
    require(::pread(replacement.descriptor, &value, 1, 0) == 0, "child used replaced stdio number");
    take(service.shutdown_until(deadline()));
}

IoCompletion next_completion(PipeService& pipes) {
    std::array<IoCompletion, 1> completed;
    require(take(pipes.wait_completions(completed, deadline())) == 1, "missing Pipe completion");
    return std::move(completed[0]);
}

int binding_child(int count, int inherited_mask) {
    const int status = child_probe(inherited_mask);
    if (status != 0) {
        return status;
    }
    auto pipes = take(PipeService::create());
    auto endpoints = take(take_inherited_pipes(pipes));
    require(endpoints.size() == static_cast<std::size_t>(count), "wrong inherited Pipe count");
    for (auto& endpoint : endpoints) {
        auto* writer = std::get_if<PipeWriter>(&endpoint.endpoint);
        require(writer != nullptr, "wrong inherited Pipe direction");
        require(endpoint.slot >= first_pipe_slot && endpoint.slot < first_pipe_slot + count,
                "wrong inherited Pipe slot");
        IoBuffer token(1);
        token.bytes()[0] = static_cast<std::byte>(endpoint.slot - first_pipe_slot);
        take(pipes.submit_write(*writer, std::move(token), deadline()));
        require(next_completion(pipes).status == IoStatus::Completed, "inherited Pipe write failed");
        writer->close();
    }
    take(pipes.shutdown_until(deadline()));
    return 0;
}

void bindings(const std::filesystem::path& executable) {
    Audit audit;
    std::array<TempFile, stream_count> files;
    auto service = take(ProcessService::create());
    // Boundary counts straddle the highest bootstrap target. Exercise actual
    // inherited stdio copies alongside handoff, not only captured output.
    for (int count : {1, 63, 64}) {
        for (int present = 0; present < (1 << stream_count); ++present) {
            close_stdio();
            for (int stream = 0; stream < stream_count; ++stream) {
                const int source = files[stream].descriptor;
                require(::ftruncate(source, 0) == 0, "reset binding data");
                require(::lseek(source, 0, SEEK_SET) == 0, "reset binding offset");
                if (stream == STDIN_FILENO) {
                    require(::write(source, "I", 1) == 1, "binding input");
                    require(::lseek(source, 0, SEEK_SET) == 0, "rewind binding input");
                }
                if (present & (1 << stream)) {
                    require(::dup2(source, stream) == stream, "binding stdio");
                    require(::fcntl(stream, F_SETFD, FD_CLOEXEC) == 0, "binding CLOEXEC");
                }
            }
            auto pipes = take(PipeService::create());
            std::vector<Pipe> endpoints;
            std::vector<PipeBinding> handoffs;
            endpoints.reserve(count);
            handoffs.reserve(count);
            for (int index = 0; index < count; ++index) {
                endpoints.push_back(take(pipes.create_pipe()));
                handoffs.push_back({first_pipe_slot + static_cast<std::uint32_t>(index),
                                    &endpoints.back().writer});
            }
            auto options = probe(executable, present);
            options.arguments = {"bindings-child", std::to_string(count), std::to_string(present)};
            auto child = take(spawn_with_pipes(service, pipes, options, handoffs));
            for (int index = 0; index < count; ++index) {
                take(pipes.submit_read(endpoints[index].reader, IoBuffer(1), deadline()));
                auto data = next_completion(pipes);
                require(data.status == IoStatus::Completed && data.transferred == 1 &&
                        data.buffer.bytes()[0] == static_cast<std::byte>(index), "Pipe mapping corrupted");
                take(pipes.submit_read(endpoints[index].reader, IoBuffer(1), deadline()));
                require(next_completion(pipes).status == IoStatus::EndOfStream, "Pipe writer leaked");
            }
            expect_exit(child);
            take(service.forget(child.id()));
            take(pipes.shutdown_until(deadline()));
            for (int stream = 0; stream < stream_count; ++stream) {
                const int flags = ::fcntl(stream, F_GETFD);
                require((present & (1 << stream)) ? flags == FD_CLOEXEC : flags == -1 && errno == EBADF,
                        "handoff changed parent stdio");
                if (stream != STDIN_FILENO) {
                    char value = 0;
                    const auto bytes = ::pread(files[stream].descriptor, &value, 1, 0);
                    require((present & (1 << stream)) ? bytes == 1 && value == '0' + stream : bytes == 0,
                            "handoff output identity");
                }
            }
        }
    }
    take(service.shutdown_until(deadline()));
}

void exhaustion(const std::filesystem::path& executable) {
    Audit audit;
    std::array<TempFile, stream_count> files;
    auto service = take(ProcessService::create());
    for (int stream = 0; stream < stream_count; ++stream) {
        require(::dup2(files[stream].descriptor, stream) == stream, "exhaustion stdio setup");
    }
    rlimit original{};
    require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "read rlimit");
    require(original.rlim_cur >= exhausted_descriptor_limit, "fixture needs descriptor limit >= 192");
    const rlimit reduced{exhausted_descriptor_limit, original.rlim_max};
    require(::setrlimit(RLIMIT_NOFILE, &reduced) == 0, "limit descriptors");
    std::vector<int> fillers;
    for (;;) {
        const int fd = ::fcntl(files[0].descriptor, F_DUPFD_CLOEXEC, stream_count);
        if (fd < 0) {
            require(errno == EMFILE, "fill descriptor table");
            break;
        }
        fillers.push_back(fd);
    }
    for (int free_slots = 0; free_slots < stream_count; ++free_slots) {
        if (free_slots != 0) {
            (void)::close(fillers.back());
            fillers.pop_back();
        }
        auto failed = service.spawn(probe(executable, 7));
        require(!failed && failed.error().is(ErrorCode::ResourceExhausted) &&
                failed.error().is_operation(ErrorOperation::Spawn) && failed.error().native.value() == EMFILE,
                "partial snapshot failure not reported");
        std::vector<int> reclaimed;
        for (int slot = 0; slot < free_slots; ++slot) {
            const int fd = ::fcntl(files[0].descriptor, F_DUPFD_CLOEXEC, stream_count);
            require(fd >= stream_count, "partial snapshot leaked");
            reclaimed.push_back(fd);
        }
        require(::fcntl(files[0].descriptor, F_DUPFD_CLOEXEC, stream_count) == -1 && errno == EMFILE,
                "fixture failed to exhaust descriptors");
        for (int fd : reclaimed) {
            (void)::close(fd);
        }
    }
    require(::setrlimit(RLIMIT_NOFILE, &original) == 0, "restore descriptors");
    for (int fd : fillers) {
        (void)::close(fd);
    }
    require(::write(files[0].descriptor, "I", 1) == 1, "retry input");
    require(::lseek(files[0].descriptor, 0, SEEK_SET) == 0, "rewind retry input");
    auto child = take(service.spawn(probe(executable, 7)));
    expect_exit(child);
    take(service.shutdown_until(deadline()));
}

void spawn_failure(const std::filesystem::path& executable) {
    Audit audit;
    std::array<TempFile, stream_count> files;
    auto service = take(ProcessService::create());
    for (int stream = 0; stream < stream_count; ++stream) {
        require(::dup2(files[stream].descriptor, stream) == stream, "failed spawn stdio");
        require(::fcntl(stream, F_SETFD, FD_CLOEXEC) == 0, "failed spawn CLOEXEC");
    }
    auto descriptor_set = [] {
        std::vector<int> descriptors;
        for (int fd = 0; fd < exhausted_descriptor_limit; ++fd) {
            if (::fcntl(fd, F_GETFD) >= 0) {
                descriptors.push_back(fd);
            }
        }
        return descriptors;
    };
    const auto baseline = descriptor_set();
    auto options = probe(executable, 7);
    options.executable = std::filesystem::temp_directory_path() /
        ("ov-missing-executable-" + std::to_string(::getpid()));
    require(!std::filesystem::exists(options.executable), "missing-executable fixture exists");
    auto failed = service.spawn(options);
    require(!failed && failed.error().is(ErrorCode::NotFound), "native spawn did not fail");
    require(descriptor_set() == baseline, "failed native spawn leaked snapshots");
    for (int stream = 0; stream < stream_count; ++stream) {
        require(::fcntl(stream, F_GETFD) == FD_CLOEXEC, "failed spawn changed parent stdio");
    }
    take(service.shutdown_until(deadline()));
}

void producers() {
    Audit audit;
    const auto name = "fdstage2_" + std::to_string(::getpid());
    const auto path = std::filesystem::temp_directory_path() / name;
    close_stdio();
    audit.check_missing = true;
    auto checked = [&](auto action) {
        const auto before = audit.acquisitions.load();
        action();
        require(audit.acquisitions.load() > before, "producer did not acquire coordinator");
        require(!audit.exposed_stdio && missing_stdio(), "producer published transient stdio");
    };
    checked([&] {
        auto service = take(ProcessService::create());
    });
    checked([&] {
        auto service = take(PipeService::create());
    });
    auto pipes = take(PipeService::create());
    checked([&] {
        auto pipe = take(pipes.create_pipe());
    });
    checked([&] {
        auto socket = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    });
    checked([&] {
        auto poller = take(SocketPoller::create());
    });
    checked([&] {
        auto lock = take(FileLock::create(path));
    });
    checked([&] {
        auto lock = take(FileLock::open(path));
    });
    std::filesystem::remove(path);
    checked([&] {
        auto memory = take(SharedMemory::create(name, mapping_bytes));
    });
    checked([&] {
        auto memory = take(SharedMemory::open(name, SharedMemoryAccess::ReadWrite));
    });
    take(SharedMemory::unlink_name(name));
    std::optional<NotificationReceiver> receiver;
    checked([&] {
        receiver.emplace(take(NotificationReceiver::create(name)));
    });
    checked([&] {
        auto other = take(NotificationReceiver::open(name));
    });
    checked([&] {
        auto sender = take(NotificationSender::open(name));
    });
    take(NotificationReceiver::unlink_name(name));
    take(pipes.shutdown_until(deadline()));
}

void contention(const std::filesystem::path& executable) {
    Audit audit;
    auto service = take(ProcessService::create());
    auto listener = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    take(listener.bind(IpEndpoint{IPv4Address{{127, 0, 0, 1}}, 0}));
    take(listener.listen(1));
    auto client = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    take(client.connect_until(take(listener.local_address()), deadline()));
    close_stdio();
    std::future<Result<Process>> waiting_spawn;
    {
        auto held = take(vinc::osal::detail::ResourceGuard::acquire_until(deadline()));
        auto attempt = listener.try_accept();
        require(!attempt && attempt.error().is(ErrorCode::WouldBlock), "try_accept waited for coordinator");
        auto timed = listener.accept_until(std::chrono::steady_clock::now() + contention_probe);
        require(!timed && timed.error().is(ErrorCode::Timeout), "ready accept ignored coordination deadline");
        std::stop_source stop;
        auto cancelled = std::async(std::launch::async, [&] {
            return listener.accept_until(Deadline::max(), stop.get_token());
        });
        stop.request_stop();
        auto stopped = cancelled.get();
        require(!stopped && stopped.error().is(ErrorCode::Cancelled), "accept cancellation lost");
        auto poller = SocketPoller::create(1, std::chrono::steady_clock::now() + contention_probe);
        require(!poller && poller.error().is(ErrorCode::Timeout), "poller coordination deadline lost");
        // Model a participating native acquisition temporarily occupying fd 1.
        const int transient = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        require(transient == STDIN_FILENO, "transient low-fd fixture");
        require(::dup2(transient, STDOUT_FILENO) == STDOUT_FILENO, "transient stdout");
        waiting_spawn = std::async(std::launch::async, [&] {
            return service.spawn(probe(executable, 0));
        });
        require(waiting_spawn.wait_for(contention_probe) == std::future_status::timeout,
                "spawn observed native acquisition before relocation/cleanup");
        close_stdio();
    }
    auto child = take(waiting_spawn.get());
    expect_exit(child);
    // The queued connection survives failed attempts; retry with a free owner.
    auto accepted = take(listener.accept_until(std::chrono::steady_clock::now()));
    require(missing_stdio(), "accept/pidfd occupied parent stdio");
    take(service.shutdown_until(deadline()));
}

void late_binding() {
    auto socket = take(Socket::open(AddressFamily::IPv4, SocketType::Stream));
    vinc::osal::detail::ResourceCoordinator replacement;
    auto bound = vinc::osal::detail::bind_resource_coordination(replacement.services());
    require(!bound && bound.error().is(ErrorCode::CoordinatorInUse),
            "real FD acquisition did not seal standalone coordinator");
}
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "child") {
        return child_probe(std::stoi(argv[2]));
    }
    if (argc == 4 && std::string_view(argv[1]) == "bindings-child") {
        return binding_child(std::stoi(argv[2]), std::stoi(argv[3]));
    }
    if (argc != 2) {
        return 2;
    }
    const auto executable = std::filesystem::absolute(argv[0]);
    const int diagnostic = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, fixture_descriptor_floor);
    const pid_t child = ::fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        (void)::setpgid(0, 0);
        try {
            const std::string_view test(argv[1]);
            if (test == "matrix") {
                matrix(executable);
            } else if (test == "reuse") {
                reuse(executable);
            } else if (test == "exhaustion") {
                exhaustion(executable);
            } else if (test == "producers") {
                producers();
            } else if (test == "contention") {
                contention(executable);
            } else if (test == "bindings") {
                bindings(executable);
            } else if (test == "late") {
                late_binding();
            } else if (test == "spawn-failure") {
                spawn_failure(executable);
            } else {
                throw std::runtime_error("unknown fixture");
            }
            ::_exit(0);
        } catch (const std::exception& failure) {
            ::dprintf(diagnostic, "FAIL %s: %s\n", argv[1], failure.what());
            ::_exit(1);
        }
    }
    (void)::setpgid(child, child);
    (void)::close(diagnostic);
    const auto end = std::chrono::steady_clock::now() + supervisor_timeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < end) {
        siginfo_t observed{};
        if (::waitid(P_PID, static_cast<id_t>(child), &observed, WEXITED | WNOHANG | WNOWAIT) == 0 &&
            observed.si_pid == child) {
            // Keep the unreaped leader's PID reserved while cleaning its group.
            (void)::kill(-child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        std::this_thread::sleep_for(supervisor_poll);
    }
    (void)::kill(-child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    std::cerr << "FAIL: isolated fixture exceeded supervisor deadline\n";
    return 1;
}
