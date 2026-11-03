// Replace only kevent64 at compile time to exercise the actual Mac monitor's
// registration, decoding and error paths. No production injection hook exists.
#include <sys/event.h>
#include <array>
#include <cerrno>
#include <iostream>
#include <stdexcept>

int controlled_kevent64(int, const struct kevent64_s*, int, struct kevent64_s*, int,
                       unsigned int, const struct timespec*);
#define kevent64 controlled_kevent64
#include "../src/macos/monitor.cpp"
#undef kevent64

namespace {
using namespace vinc::osal;
constexpr std::uint64_t full_width_id = 0xf123456789abcdef;
constexpr pid_t fake_pid = 123;
std::array<kevent64_s, detail::ProcessExitEvents::capacity> supplied{};
int supplied_count = 0;
int syscall_error = 0;
bool saw_timed_wait = false;
bool saw_untimed_wait = false;
bool saw_watch = false;
bool saw_wake = false;

void require(bool condition, const char* message) {
    if (!condition) {
        // Assertions can run inside the production noexcept native boundary.
        // Fail the fixture without unwinding through it or generating a crash UI.
        std::cerr << message << std::endl;
        std::_Exit(1);
    }
}

void set_exit(std::size_t index, std::uint64_t id) {
    EV_SET64(&supplied[index], fake_pid, EVFILT_PROC, EV_ONESHOT, NOTE_EXIT, 0, id, 0, 0);
}
}

int controlled_kevent64(int, const struct kevent64_s* changes, int count,
                       struct kevent64_s* events, int capacity, unsigned int flags,
                       const struct timespec* timeout) {
    require(flags == 0, "unexpected syscall flags");
    if (count != 0) {
        require(count == 1 && !events, "unexpected registration batch");
        const auto& change = changes[0];
        if (change.filter == EVFILT_PROC) {
            require(change.ident == fake_pid && change.udata == full_width_id, "watch identity truncated");
            require(change.flags == (EV_ADD | EV_ONESHOT | EV_UDATA_SPECIFIC), "watch key can retag old events");
            require(change.fflags == NOTE_EXIT, "incorrect process filter");
            saw_watch = true;
        } else {
            require(change.filter == EVFILT_USER && change.udata == 0, "invalid user wake registration");
            if (change.fflags == NOTE_TRIGGER) {
                saw_wake = true;
            }
        }
        return 0;
    }
    require(capacity == static_cast<int>(supplied.size()), "unbounded event batch");
    if (timeout) {
        require(timeout->tv_sec == 0 && timeout->tv_nsec == 50'000'000, "fallback interval changed");
        saw_timed_wait = true;
    } else {
        saw_untimed_wait = true;
    }
    if (syscall_error) {
        errno = syscall_error;
        return -1;
    }
    for (int i = 0; i < supplied_count; ++i) {
        events[i] = supplied[static_cast<std::size_t>(i)];
    }
    return supplied_count;
}

int main() {
    try {
        auto created = detail::NativeMonitor::create();
        require(bool(created), "create native monitor");
        auto monitor = std::move(*created);
        auto watched = detail::watch_process(monitor->implementation(), fake_pid, ProcessId{full_width_id});
        require(bool(watched) && saw_watch, "watch registration missing");
        monitor->wake();
        require(saw_wake, "wake did not use kevent64");

        set_exit(0, full_width_id);
        EV_SET64(&supplied[1], 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0);
        EV_SET64(&supplied[2], fake_pid, EVFILT_PROC, 0, 0, 0, full_width_id, 0, 0);
        supplied_count = 3;
        auto batch = monitor->wait(false);
        require(batch && batch->count == 1 && batch->ids[0].value == full_width_id,
                "exit decode lost token or treated unrelated event as exit");

        for (std::size_t i = 0; i < supplied.size(); ++i) {
            set_exit(i, full_width_id + i);
        }
        supplied_count = static_cast<int>(supplied.size());
        batch = monitor->wait(true);
        require(batch && batch->count == supplied.size(), "full batch truncated");
        for (std::size_t i = 0; i < supplied.size(); ++i) {
            require(batch->ids[i].value == full_width_id + i, "batch identity mismatch");
        }

        // A consumed exit followed by EV_ERROR must remain a failure so that the
        // common monitor-error path takes responsibility for continued polling.
        set_exit(0, full_width_id);
        EV_SET64(&supplied[1], 1, EVFILT_USER, EV_ERROR, 0, EIO, 0, 0, 0);
        supplied_count = 2;
        batch = monitor->wait(false);
        require(!batch && batch.error() == detail::native_error(ErrorOperation::Observe, EIO),
                "mixed batch swallowed native error");

        syscall_error = EINTR;
        batch = monitor->wait(false);
        require(batch && batch->count == 0, "interruption fabricated an exit");
        syscall_error = EBADF;
        batch = monitor->wait(false);
        require(!batch && batch.error() == detail::native_error(ErrorOperation::Observe, EBADF),
                "syscall error lost native diagnostics");
        require(saw_timed_wait && saw_untimed_wait, "wait modes not covered");
        std::cout << "macOS exit monitor contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
