// Native fault probes run in isolated children, without modifying the test
// launcher's limits, signal disposition, or inherited descriptors.
#include <vinc/osal/notification.hpp>
#include <cerrno>
#include <array>
#include <csignal>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace vinc::osal;
namespace {
constexpr int fixture_minimum_fd = 64;
constexpr int descriptor_scan_limit = 512;
constexpr int lifecycle_iterations = 64;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class T> T take(vinc::Result<T> result) {
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
std::string name() {
    return "np" + std::to_string(::getpid());
}
std::string native_path() {
    return "/tmp/ov-osal-notify-" + std::to_string(::geteuid()) + "/" + name();
}
struct Cleanup {
    ~Cleanup() {
        (void)NotificationReceiver::unlink_name(name());
    }
};
struct Limit {
    rlimit original{};
    std::vector<int> fillers;
    bool changed = false;
    explicit Limit(int free_descriptors) {
        require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "getrlimit");
        // Fill actual holes rather than assuming fd 3 is free under CTest.
        int last = -1;
        while (last < fixture_minimum_fd) {
            last = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            require(last >= 0, "fill fd table");
            fillers.push_back(last);
        }
        rlimit restricted = original;
        restricted.rlim_cur = static_cast<rlim_t>(last + 1 + free_descriptors);
        require(::setrlimit(RLIMIT_NOFILE, &restricted) == 0, "restrict fd table");
        changed = true;
    }
    ~Limit() {
        if (changed) {
            (void)::setrlimit(RLIMIT_NOFILE, &original);
        }
        for (const int descriptor : fillers) {
            (void)::close(descriptor);
        }
    }
};
int fd_count();

void rollback(int headroom) {
    Cleanup cleanup;
    vinc::Result<NotificationReceiver> result;
    {
        Limit restricted(headroom);
        result = NotificationReceiver::create(name());
    }
    require(!result && result.error().is(ErrorCode::ResourceExhausted), "create should exhaust fds");
    require(result.error().native.code == EMFILE, "preserve EMFILE");
    require(result.error().operation == static_cast<std::int32_t>(ErrorOperation::CreateNotification),
            "create operation");
    struct stat info{};
    require(::lstat(native_path().c_str(), &info) != 0 && errno == ENOENT, "rollback left FIFO behind");
    auto receiver = take(NotificationReceiver::create(name()));
    vinc::Result<NotificationSender> opened;
    {
        Limit restricted(1);
        opened = NotificationSender::open(name());
    }
    require(!opened && opened.error().is(ErrorCode::ResourceExhausted), "open should exhaust fds");
    auto sender = take(NotificationSender::open(name()));
    ok(sender.notify());
    const int before = fd_count();
    require(::lstat(native_path().c_str(), &info) == 0, "inspect existing FIFO");
    vinc::Result<NotificationReceiver> receiver_open;
    {
        Limit restricted(headroom);
        receiver_open = NotificationReceiver::open(name());
    }
    require(!receiver_open && receiver_open.error().is(ErrorCode::ResourceExhausted),
            "receiver open should exhaust fds");
    require(receiver_open.error().native.code == EMFILE, "receiver open preserves EMFILE");
    require(receiver_open.error().operation == static_cast<std::int32_t>(ErrorOperation::OpenNotification),
            "receiver open operation");
    require(fd_count() == before, "failed receiver open leaked descriptors");
    struct stat after{};
    require(::lstat(native_path().c_str(), &after) == 0 && info.st_dev == after.st_dev && info.st_ino == after.st_ino,
            "failed receiver open removed or replaced FIFO");
    // Failed open must preserve the existing receiver and its queued data.
    ok(receiver.wait_until(std::chrono::steady_clock::now()));
    auto successor = take(NotificationReceiver::open(name()));
    ok(receiver.close());
    ok(sender.notify());
    ok(successor.wait_until(std::chrono::steady_clock::now()));
}
volatile sig_atomic_t signals_seen = 0;
void handler(int) {
    signals_seen = 1;
}
void broken_pipe(int mode) {
    Cleanup cleanup;
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    if (mode == 1) {
        action.sa_handler = handler;
    } else if (mode == 2) {
        action.sa_handler = SIG_IGN;
    }
    require(::sigaction(SIGPIPE, &action, nullptr) == 0, "set signal policy");
    sigset_t pipe_set;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    if (mode == 3) {
        require(::sigprocmask(SIG_BLOCK, &pipe_set, nullptr) == 0, "block SIGPIPE");
        require(::raise(SIGPIPE) == 0, "raise pre-existing SIGPIPE");
    }
    sigset_t before;
    require(::sigprocmask(SIG_BLOCK, nullptr, &before) == 0, "read signal mask");
    auto receiver = take(NotificationReceiver::create(name()));
    auto sender = take(NotificationSender::open(name()));
    ok(receiver.close());
    const auto result = sender.notify();
    require(!result && result.error().is(ErrorCode::PeerClosed), "expected PeerClosed");
    require(result.error().native.code == EPIPE, "expected native EPIPE");
    require(signals_seen == 0, "notify invoked process SIGPIPE handler");
    sigset_t after;
    require(::sigprocmask(SIG_BLOCK, nullptr, &after) == 0, "read restored mask");
    require(sigismember(&before, SIGPIPE) == sigismember(&after, SIGPIPE), "mask changed");
    struct sigaction after_action{};
    require(::sigaction(SIGPIPE, nullptr, &after_action) == 0, "read signal policy");
    require(after_action.sa_handler == action.sa_handler, "signal disposition changed");
    sigset_t pending;
    require(::sigpending(&pending) == 0, "read pending signals");
    require(sigismember(&pending, SIGPIPE) == (mode == 3 ? 1 : 0), "wrong pending signal state");
}
int fd_count() {
    int count = 0;
    for (int descriptor = 0; descriptor < descriptor_scan_limit; ++descriptor) {
        if (::fcntl(descriptor, F_GETFD) >= 0) {
            ++count;
        }
    }
    return count;
}
void lifetime() {
    Cleanup cleanup;
    const int before = fd_count();
    for (int iteration = 0; iteration < lifecycle_iterations; ++iteration) {
        std::array<bool, descriptor_scan_limit> inherited{};
        for (int descriptor = 0; descriptor < descriptor_scan_limit; ++descriptor) {
            inherited[descriptor] = ::fcntl(descriptor, F_GETFD) >= 0;
        }
        auto receiver = take(NotificationReceiver::create(name()));
        auto opened = take(NotificationReceiver::open(name()));
        auto sender = take(NotificationSender::open(name()));
        for (int descriptor = 0; descriptor < descriptor_scan_limit; ++descriptor) {
            const auto flags = ::fcntl(descriptor, F_GETFD);
            if (flags >= 0 && !inherited[descriptor]) {
                require((flags & FD_CLOEXEC) != 0, "notification descriptor can leak across exec");
            }
        }
        struct stat info{};
        require(::lstat(native_path().c_str(), &info) == 0, "inspect FIFO");
        require(S_ISFIFO(info.st_mode) && (info.st_mode & 0077) == 0, "private FIFO mode");
        ok(NotificationReceiver::unlink_name(name()));
        ok(receiver.close());
        ok(sender.notify());
        ok(opened.wait_until(std::chrono::steady_clock::now()));
    }
    require(fd_count() == before, "notification leaked descriptors");
}
void stdio_isolation() {
    Cleanup cleanup;
    // All acquired descriptors must be relocated out of the caller's stdio.
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        (void)::close(descriptor);
    }
    auto receiver = take(NotificationReceiver::create(name()));
    auto opened = take(NotificationReceiver::open(name()));
    auto sender = take(NotificationSender::open(name()));
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        require(::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF, "occupied caller stdio");
    }
    ok(sender.notify());
    ok(receiver.close());
    ok(opened.wait_until(std::chrono::steady_clock::now()));
}
void invalid_namespace() {
    Cleanup cleanup;
    // First establish the private directory through the public API.
    auto receiver = take(NotificationReceiver::create(name()));
    ok(receiver.close());
    ok(NotificationReceiver::unlink_name(name()));
    require(::symlink("/dev/null", native_path().c_str()) == 0, "create controlled symlink");
    auto opened = NotificationSender::open(name());
    auto receiver_open = NotificationReceiver::open(name());
    auto removed = NotificationReceiver::unlink_name(name());
    const bool rejected = !opened && opened.error().is(ErrorCode::InvalidArgument) &&
                          !receiver_open && receiver_open.error().is(ErrorCode::InvalidArgument) &&
                          !removed && removed.error().is(ErrorCode::InvalidArgument);
    (void)::unlink(native_path().c_str());
    require(rejected, "followed or removed non-FIFO object");
}
void interrupted_wait() {
    using namespace std::chrono_literals;
    constexpr auto interrupted_timeout = 30ms;
    constexpr auto signal_interval = 1ms;
    Cleanup cleanup;
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    require(::sigaction(SIGUSR1, &action, nullptr) == 0, "install interrupt handler");
    auto receiver = take(NotificationReceiver::create(name()));
    const auto waiting_thread = ::pthread_self();
    std::jthread interrupter([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            (void)::pthread_kill(waiting_thread, SIGUSR1);
            std::this_thread::sleep_for(signal_interval);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + interrupted_timeout;
    const auto result = receiver.wait_until(deadline);
    interrupter.request_stop();
    interrupter.join();
    require(signals_seen != 0, "wait was not interrupted");
    require(!result && result.error().is(ErrorCode::Timeout), "EINTR must preserve timeout contract");
    require(std::chrono::steady_clock::now() >= deadline, "EINTR caused early timeout");
}
}

int main() {
    constexpr int test_cases = 10;
    for (int test = 0; test < test_cases; ++test) {
        const auto child = ::fork();
        if (child == 0) {
            try {
                if (test < 2) {
                    rollback(test + 1);
                } else if (test < 6) {
                    broken_pipe(test - 2);
                } else if (test == 6) {
                    lifetime();
                } else if (test == 7) {
                    invalid_namespace();
                } else if (test == 8) {
                    stdio_isolation();
                } else {
                    interrupted_wait();
                }
                ::_exit(0);
            } catch (const std::exception& error) {
                std::cerr << "case " << test << ": " << error.what() << '\n';
                ::_exit(1);
            }
        }
        if (child < 0) {
            std::cerr << "fork failed\n";
            return 1;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::cerr << "FAIL: native notification case " << test << ", status " << status << '\n';
            return 1;
        }
    }
    std::cout << "PASS: native rollback, SIGPIPE policies, lifetime, namespace, stdio isolation\n";
    return 0;
}
