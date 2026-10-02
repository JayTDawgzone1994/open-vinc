// Thread-owned mutex policy composed from OSAL FileLock. No native OS handles,
// platform headers, conditional backends, or Param/Module knowledge here.
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/osal/file_lock.hpp>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::concurrency {
namespace detail {
struct InterprocessMutexState {
    osal::FileLock file_lock;
    // Protects the local reservation, owner thread and retained FileLock guard.
    std::mutex local_mutex;
    std::condition_variable_any local_changed;
    // Reserve the local thread slot before blocking on FileLock. Other local
    // threads wait here instead of concurrently acquiring the same file handle.
    bool occupied = false;
    std::thread::id owner_thread;
    std::optional<osal::FileLockGuard> file_guard;
};
}

namespace {
using State = detail::InterprocessMutexState;

Error error(ErrorCode code, Operation operation = Operation::Lock) {
    return {code, operation, {}};
}

Error native_error(const osal::OsError& failure, Operation operation) {
    return {failure.code, operation, failure};
}

// Caller holds local_mutex. Release the reservation only after native unlock
// succeeds, or while rolling back an acquisition that never returned a guard.
void release_local_slot(State& state) {
    state.occupied = false;
    state.owner_thread = {};
    state.local_changed.notify_all();
}

// Owns rollback after local reservation succeeds. The referenced State is kept
// alive by the acquiring call; native failure or unwinding releases the local slot.
struct PendingReservation {
    State& state;
    bool committed = false;

    ~PendingReservation() {
        if (!committed) {
            std::lock_guard lock(state.local_mutex);
            release_local_slot(state);
        }
    }
};

// Reserve only the local thread slot. No OS lock is acquired while local_mutex
// is held; the caller must install PendingReservation immediately on success.
Result<void> try_reserve_local_slot(State& state) {
    std::lock_guard lock(state.local_mutex);
    if (state.occupied) {
        return std::unexpected(error(
            state.owner_thread == std::this_thread::get_id() ? ErrorCode::Busy : ErrorCode::WouldBlock));
    }
    state.occupied = true;
    state.owner_thread = std::this_thread::get_id();
    return {};
}

Result<void> reserve_local_slot_until(State& state, osal::Deadline deadline, std::stop_token stop) {
    std::unique_lock lock(state.local_mutex);
    if (stop.stop_requested()) {
        return std::unexpected(error(ErrorCode::Cancelled));
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(error(ErrorCode::Timeout));
    }

    if (state.occupied && state.owner_thread == std::this_thread::get_id()) {
        return std::unexpected(error(ErrorCode::Busy));
    }

    state.local_changed.wait_until(lock, stop, deadline, [&] {
        return !state.occupied;
    });
    if (stop.stop_requested()) {
        return std::unexpected(error(ErrorCode::Cancelled));
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(error(ErrorCode::Timeout));
    }

    state.occupied = true;
    state.owner_thread = std::this_thread::get_id();
    return {};
}

// Transfer the acquired OS lock into State before the public guard is returned.
// The caller's reservation still owns rollback until this transfer completes.
void retain_file_guard(State& state, osal::FileLockGuard&& guard) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    {
        std::lock_guard lock(state.local_mutex);
        state.file_guard.emplace(std::move(guard));
    }
}
}

InterprocessMutex::InterprocessMutex() noexcept = default;
InterprocessMutex::~InterprocessMutex() = default;
InterprocessMutex::InterprocessMutex(InterprocessMutex&&) noexcept = default;
InterprocessMutex& InterprocessMutex::operator=(InterprocessMutex&&) noexcept = default;

InterprocessMutex::InterprocessMutex(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {
}

InterprocessMutex::operator bool() const noexcept {
    return bool(state_);
}

Result<InterprocessMutex> InterprocessMutex::create(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::create(path);
    if (!file) {
        return std::unexpected(native_error(file.error(), Operation::Create));
    }

    state->file_lock = std::move(*file);
    return InterprocessMutex(std::move(state));
}

Result<InterprocessMutex> InterprocessMutex::open(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::open(path);
    if (!file) {
        return std::unexpected(native_error(file.error(), Operation::Open));
    }

    state->file_lock = std::move(*file);
    return InterprocessMutex(std::move(state));
}

void InterprocessMutex::close() noexcept {
    state_.reset();
}

Result<InterprocessMutexGuard> InterprocessMutex::try_lock() const {
    // Retain State for acquisition rollback and the guard returned on success.
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    auto reserved = try_reserve_local_slot(*state);
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    PendingReservation reservation{*state};
    auto guard = state->file_lock.try_lock();
    if (!guard) {
        return std::unexpected(native_error(guard.error(), Operation::Lock));
    }

    retain_file_guard(*state, std::move(*guard));
    reservation.committed = true;
    return InterprocessMutexGuard(std::move(state));
}

Result<InterprocessMutexGuard> InterprocessMutex::lock_until(osal::Deadline deadline,
                                                             std::stop_token stop) const {
    // Retain State for acquisition rollback and the guard returned on success.
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    auto reserved = reserve_local_slot_until(*state, deadline, stop);
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    PendingReservation reservation{*state};
    // One original deadline/token covers BOTH local and cross-process waits.
    auto guard = state->file_lock.lock_until(deadline, stop);
    if (!guard) {
        return std::unexpected(native_error(guard.error(), Operation::Lock));
    }

    retain_file_guard(*state, std::move(*guard));
    reservation.committed = true;
    return InterprocessMutexGuard(std::move(state));
}

InterprocessMutexGuard::InterprocessMutexGuard() noexcept = default;

InterprocessMutexGuard::InterprocessMutexGuard(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {
}

InterprocessMutexGuard::~InterprocessMutexGuard() {
    release_or_terminate();
}

InterprocessMutexGuard::InterprocessMutexGuard(InterprocessMutexGuard&& other) noexcept
    : state_(std::move(other.state_)) {
}

InterprocessMutexGuard& InterprocessMutexGuard::operator=(InterprocessMutexGuard&& other) noexcept {
    if (this != &other) {
        release_or_terminate();
        state_ = std::move(other.state_);
    }
    return *this;
}

InterprocessMutexGuard::operator bool() const noexcept {
    return bool(state_);
}

Result<void> InterprocessMutexGuard::unlock() noexcept {
    if (!state_) {
        return {};
    }

    auto state = state_;
    std::lock_guard lock(state->local_mutex);
    if (state->owner_thread != std::this_thread::get_id()) {
        return std::unexpected(error(ErrorCode::InvalidArgument, Operation::Unlock));
    }

    // Keep protected writes before releasing the native lock.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // Native unlock failure retains both levels of ownership so the caller can retry.
    auto result = state->file_guard->unlock();
    if (!result) {
        return std::unexpected(native_error(result.error(), Operation::Unlock));
    }

    state->file_guard.reset();
    release_local_slot(*state);
    state_.reset();
    return {};
}

void InterprocessMutexGuard::release_or_terminate() noexcept {
    if (!unlock()) {
        std::terminate();
    }
}
} // namespace vinc::concurrency
