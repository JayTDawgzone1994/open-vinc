// Manage stable watch identities above the platform readiness backend.
// Check that a watch is still live before delivering events from an OS wait batch.

#include "socket_native.hpp"
#include <algorithm>
#include <atomic>
#include <limits>

namespace vinc::osal::detail {
struct SocketPollerState {
    struct Entry {
        SocketRegistration registration;
        std::weak_ptr<SocketState> socket;
        SocketHandle handle;
        std::uint64_t tag;
    };
    std::unique_ptr<NativeSocketPoller> native;
    std::vector<Entry> entries;
    std::vector<NativeSocketEvent> events;
    std::size_t maximum;
    void prune() noexcept {
        std::erase_if(entries, [&](const Entry& entry) {
            auto socket = entry.socket.lock();
            if (socket && socket->handle == entry.handle) return false;
            // Closing a descriptor automatically removes POSIX kernel watches.
            // Do not delete by its old number: a new socket may reuse it.
            native->remove(invalid_socket, entry.registration.value);
            return true;
        });
    }
};
}
namespace vinc::osal {
namespace {
bool valid_interests(SocketEvents interests) {
    return interests != SocketEvents::None && (static_cast<unsigned>(interests) & ~3u) == 0;
}
Result<SocketRegistration> next_registration() {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::RegisterSocket});
    } while (!sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return SocketRegistration{value};
}
}
SocketPoller::SocketPoller(std::unique_ptr<detail::SocketPollerState> state) : state_(std::move(state)) {}
SocketPoller::~SocketPoller() = default;
SocketPoller::SocketPoller(SocketPoller&&) noexcept = default;
SocketPoller& SocketPoller::operator=(SocketPoller&&) noexcept = default;
Result<SocketPoller> SocketPoller::create(std::size_t maximum, Deadline deadline, std::stop_token token) {
    if (token.stop_requested()) return std::unexpected(OsError{ErrorCode::Cancelled, Operation::InitializeService});
    if (std::chrono::steady_clock::now() >= deadline) return std::unexpected(OsError{ErrorCode::Timeout, Operation::InitializeService});
    if (!maximum || maximum > static_cast<std::size_t>(INT_MAX - 1))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::InitializeService});
    auto state = std::make_unique<detail::SocketPollerState>();
    state->entries.reserve(maximum); state->events.resize(maximum); state->maximum = maximum;
    auto native = detail::NativeSocketPoller::create(maximum, deadline, token);
    if (!native) return std::unexpected(native.error());
    state->native = std::move(*native);
    return SocketPoller(std::move(state));
}
Result<SocketRegistration> SocketPoller::add(Socket& socket, SocketEvents interests, std::uint64_t tag) {
    if (!state_) return std::unexpected(OsError{ErrorCode::Closed, Operation::RegisterSocket});
    if (!socket || !valid_interests(interests)) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::RegisterSocket});
    state_->prune();
    for (const auto& entry : state_->entries) if (entry.socket.lock() == socket.state_)
        return std::unexpected(OsError{ErrorCode::Busy, Operation::RegisterSocket});
    if (state_->entries.size() == state_->maximum) return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::RegisterSocket});
    auto registration = next_registration(); if (!registration) return std::unexpected(registration.error());
    auto added = state_->native->add(socket.state_->handle, registration->value, interests);
    if (!added) return std::unexpected(added.error());
    state_->entries.push_back({*registration, socket.state_, socket.state_->handle, tag});
    return *registration;
}
Result<void> SocketPoller::modify(SocketRegistration registration, SocketEvents interests) {
    if (!state_) return std::unexpected(OsError{ErrorCode::Closed, Operation::RegisterSocket});
    if (!valid_interests(interests)) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::RegisterSocket});
    state_->prune();
    for (auto it = state_->entries.begin(); it != state_->entries.end(); ++it) if (it->registration == registration) {
        auto result = state_->native->modify(it->handle, registration.value, interests);
        if (!result) {
            // A backend may have changed only one direction (kqueue). A failed
            // native modify invalidates the registration, never half-updates it.
            state_->native->remove(it->handle, registration.value);
            state_->entries.erase(it);
        }
        return result;
    }
    return std::unexpected(OsError{ErrorCode::NotFound, Operation::RegisterSocket});
}
Result<void> SocketPoller::remove(SocketRegistration registration) {
    if (!state_) return std::unexpected(OsError{ErrorCode::Closed, Operation::RegisterSocket});
    state_->prune();
    const auto it = std::find_if(state_->entries.begin(), state_->entries.end(), [&](const auto& entry) { return entry.registration == registration; });
    if (it == state_->entries.end()) return std::unexpected(OsError{ErrorCode::NotFound, Operation::RegisterSocket});
    state_->native->remove(it->handle, registration.value);
    state_->entries.erase(it); return {};
}
Result<std::size_t> SocketPoller::wait_until(std::span<SocketReadyEvent> output, Deadline deadline, std::stop_token token) {
    if (!state_) return std::unexpected(OsError{ErrorCode::Closed, Operation::WaitSocket});
    if (output.empty()) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::WaitSocket});
    state_->prune();
    std::stop_callback stopped(token, [this] { (void)state_->native->wake(); });
    for (;;) {
        if (token.stop_requested()) return std::unexpected(OsError{ErrorCode::Cancelled, Operation::WaitSocket});
        const auto now = std::chrono::steady_clock::now();
        int milliseconds = -1;
        if (deadline != Deadline::max()) {
            // Clamp before subtraction to avoid duration overflow for arbitrary deadlines.
            if (deadline <= now) milliseconds = 0;
            else if (deadline - std::chrono::milliseconds(INT_MAX) > now) milliseconds = INT_MAX;
            else milliseconds = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
        }
        auto count = state_->native->wait(std::span(state_->events).first(std::min(output.size(), state_->events.size())), milliseconds);
        if (token.stop_requested()) return std::unexpected(OsError{ErrorCode::Cancelled, Operation::WaitSocket});
        if (!count) {
            if (count.error().code == ErrorCode::WouldBlock) continue; // interrupted native wait
            if (count.error().code == ErrorCode::Timeout && std::chrono::steady_clock::now() < deadline) continue;
            return std::unexpected(count.error());
        }
        if (*count == 0) return std::size_t{0}; // explicit wake
        std::size_t written = 0;
        for (std::size_t i = 0; i < *count; ++i) {
            const auto& event = state_->events[i];
            auto it = std::find_if(state_->entries.begin(), state_->entries.end(), [&](const auto& entry) { return entry.registration.value == event.id; });
            if (it == state_->entries.end()) continue;
            auto socket = it->socket.lock();
            if (!socket || socket->handle != it->handle) continue;
            output[written++] = {it->registration, event.events, it->tag};
        }
        if (written) return written;
    }
}
Result<void> SocketPoller::wake() noexcept {
    if (!state_) return std::unexpected(OsError{ErrorCode::Closed, Operation::WakeSocket});
    return state_->native->wake();
}
} // namespace vinc::osal
