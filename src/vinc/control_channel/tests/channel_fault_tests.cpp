#include "channel_test_support.hpp"
#include "channel_test_hooks.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace channel_test;

enum class Injection {
    None,
    PartialApplication,
    PollFailure,
    WorkerException
};

struct HookState {
    std::mutex mutex;
    std::condition_variable changed;
    Injection injection = Injection::None;
    bool partial_sent = false;
    bool paused = false;
    bool released = false;
};

HookState hook;

const Error synthetic_poll_error{
    osal::ErrorCode::SystemError, osal::ErrorOperation::WaitSocket,
    NativeError{NativeErrorSource::WindowsGeneric, 42}};

// Declare before Service so the gate outlives its worker on success and failure.
// The worker also observes stop while paused, making assertion unwinding bounded.
class InjectionScope {
public:
    void arm(Injection injection) {
        std::lock_guard lock(hook.mutex);
        hook.injection = injection;
        hook.partial_sent = false;
        hook.paused = false;
        hook.released = false;
    }

    void wait_paused() {
        std::unique_lock lock(hook.mutex);
        check(hook.changed.wait_for(lock, operation_timeout, [] {
            return hook.paused;
        }), "worker did not reach injection boundary");
    }

    void release() {
        std::lock_guard lock(hook.mutex);
        hook.released = true;
        hook.changed.notify_all();
    }

    ~InjectionScope() {
        std::lock_guard lock(hook.mutex);
        hook.injection = Injection::None;
        hook.released = true;
        hook.changed.notify_all();
    }
};

void check_backpressure(control::Session& session) {
    auto rejected = session.send(application());
    check(!rejected && rejected.error() == Error{control::ErrorCode::Backpressure, control::ErrorOperation::Send},
          "sender exceeded the exact queue boundary");
}

void test_partial_send_count_limit() {
    InjectionScope injection;
    auto service = take(control::Service::create(1));
    auto peer = attach_raw(*service, options());
    injection.arm(Injection::PartialApplication);
    // The first frame is now active with exactly one byte written to the socket.
    check(bool(peer.session->send(application(1))), "queue first partial frame");
    injection.wait_paused();
    constexpr std::size_t queue_limit = 64;
    for (std::size_t index = 1; index < queue_limit; ++index) {
        auto frame = application();
        frame.operation = static_cast<std::uint16_t>(index + 1);
        check(bool(peer.session->send(std::move(frame))), "queue filled too early");
    }
    check_backpressure(*peer.session);
    injection.release();
    auto frames = read_applications(peer, queue_limit);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        check(frames[index].operation == index + 1, "partial sending reordered frame bytes");
    }
    check(frames.front().payload == application(1).payload, "partial frame payload corrupted");
    check(bool(peer.session->send(application())), "sent frame did not release queue capacity");
    (void)read_applications(peer, 1);
}

void test_partial_send_byte_limit() {
    InjectionScope injection;
    auto service = take(control::Service::create(1));
    auto peer = attach_raw(*service, options());
    injection.arm(Injection::PartialApplication);
    check(bool(peer.session->send(application(control::max_payload))), "queue large partial frame");
    injection.wait_paused();
    constexpr std::size_t byte_limit = 256 * 1024;
    constexpr auto full_frame_bytes = control::header_size + control::max_payload;
    constexpr auto full_frames = byte_limit / full_frame_bytes;
    constexpr auto remaining_bytes = byte_limit % full_frame_bytes;
    static_assert(remaining_bytes >= control::header_size);
    for (std::size_t index = 1; index < full_frames; ++index) {
        check(bool(peer.session->send(application(control::max_payload))), "byte budget filled too early");
    }
    check(bool(peer.session->send(application(remaining_bytes - control::header_size))), "exact byte budget rejected");
    check_backpressure(*peer.session);
    injection.release();
    auto frames = read_applications(peer, full_frames + 1);
    std::size_t received_bytes = 0;
    for (const auto& frame : frames) {
        received_bytes += control::header_size + frame.payload.size();
        check(frame.payload == application(frame.payload.size()).payload, "payload changed during partial send");
    }
    check(received_bytes == byte_limit, "byte budget accounted only for unsent tail");
    check(bool(peer.session->send(application(control::max_payload))), "sent frames did not release byte capacity");
    (void)read_applications(peer, 1);
}

void test_terminal_failure(Injection mode) {
    InjectionScope injection;
    auto service = take(control::Service::create(4));
    auto existing = attach_raw(*service, options());
    auto closing = attach_raw(*service, options());
    injection.arm(mode);
    // A real send wakes the worker if it was already inside the native wait.
    check(bool(existing.session->send(application())), "wake worker for injected failure");
    injection.wait_paused();
    closing.session->close();
    // This peer was published after the worker took its current peer snapshot.
    auto late = attach_raw(*service, options(), false);
    injection.release();
    wait_closed(*existing.session);
    wait_closed(*late.session);
    wait_closed(*closing.session);

    const auto expected = mode == Injection::PollFailure
        ? wrap_error(control::ErrorCode::Io, control::ErrorOperation::Poll, synthetic_poll_error)
        : Error{control::ErrorCode::WorkerFailed, control::ErrorOperation::Close};
    check(existing.session->error() == expected && late.session->error() == expected,
          "terminal fault was not applied to every current peer");
    check(closing.session->error() == Error{control::ErrorCode::Closed, control::ErrorOperation::Close},
          "terminal fault overwrote an earlier close");
    check_send_cause(*existing.session, expected);
    check_send_cause(*late.session, expected);
    auto drained = late.session->try_receive();
    check(!drained && drained.error() == expected, "receive lost terminal failure");
    auto [server, remote] = connected_sockets();
    auto rejected = service->attach(std::move(server), options());
    check(!rejected && rejected.error().is(control::ErrorCode::Closed), "stopped engine accepted another peer");
}
}

namespace vinc::control::test {
Result<std::size_t> try_send(osal::Socket& socket, std::span<const std::byte> bytes) {
    std::lock_guard lock(hook.mutex);
    if (hook.injection != Injection::PartialApplication || hook.released) {
        return socket.try_send(bytes);
    }

    if (hook.partial_sent) {
        return std::unexpected(Error{osal::ErrorCode::WouldBlock, osal::ErrorOperation::SendSocket});
    }

    // Only application frames are paused. Hello/heartbeat retain their normal
    // reserved send path. The fixture uses the documented big-endian v1 kind.
    const bool application_frame = bytes.size() >= header_size && bytes[6] == std::byte{0} &&
                                   bytes[7] == std::byte{static_cast<unsigned char>(Kind::Event)};
    if (application_frame) {
        auto sent = socket.try_send(bytes.first(1));
        if (sent && *sent == 1) {
            hook.partial_sent = true;
        }
        return sent;
    }

    return socket.try_send(bytes);
}

Result<std::size_t> wait_until(osal::SocketPoller& poller, std::span<osal::SocketReadyEvent> ready,
                               Deadline deadline, std::stop_token stop) {
    std::stop_callback notify_stop(stop, [] {
        hook.changed.notify_all();
    });
    std::unique_lock lock(hook.mutex);
    const bool pause = !hook.released &&
        (hook.injection == Injection::PollFailure || hook.injection == Injection::WorkerException ||
         (hook.injection == Injection::PartialApplication && hook.partial_sent));
    if (pause) {
        hook.paused = true;
        hook.changed.notify_all();
        const bool resumed = hook.changed.wait_for(lock, operation_timeout, [&] {
            return hook.released || stop.stop_requested();
        });
        if (stop.stop_requested()) {
            return std::unexpected(Error{osal::ErrorCode::Cancelled, osal::ErrorOperation::WaitSocket});
        }
        if (!resumed) {
            throw std::runtime_error("test injection gate timed out");
        }
        if (hook.injection == Injection::PollFailure) {
            return std::unexpected(synthetic_poll_error);
        }
        if (hook.injection == Injection::WorkerException) {
            throw std::runtime_error("injected worker failure");
        }
    }
    lock.unlock();
    return poller.wait_until(ready, deadline, stop);
}
}

int main() {
    try {
        test_partial_send_count_limit();
        test_partial_send_byte_limit();
        test_terminal_failure(Injection::PollFailure);
        test_terminal_failure(Injection::WorkerException);
        std::cout << "control exact capacity and terminal failure tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
