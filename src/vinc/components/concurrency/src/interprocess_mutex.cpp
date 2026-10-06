// Thread-owned mutex policy composed from OSAL FileLock. No native OS handles,
// platform headers, conditional backends, or Param/Module knowledge here.
#include <vinc/time.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/concurrency/detail/error.hpp>
#include <vinc/concurrency/detail/interprocess_mutex_state.hpp>
#include <vinc/osal/file_lock.hpp>
#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

namespace vinc::concurrency {
vinc::Error detail::translate_file_lock_error(const vinc::Error& failure, ErrorOperation operation) noexcept {
    if (failure.domain != ErrorDomain::Osal) {
        return vinc::wrap_error(ErrorCode::SystemError, operation, failure);
    }

    switch (static_cast<osal::ErrorCode>(failure.code)) {
    case osal::ErrorCode::InvalidArgument:
        return vinc::wrap_error(ErrorCode::InvalidArgument, operation, failure);
    case osal::ErrorCode::NotFound:
        return vinc::wrap_error(ErrorCode::NotFound, operation, failure);
    case osal::ErrorCode::PermissionDenied:
        return vinc::wrap_error(ErrorCode::PermissionDenied, operation, failure);
    case osal::ErrorCode::ResourceExhausted:
        return vinc::wrap_error(ErrorCode::ResourceExhausted, operation, failure);
    case osal::ErrorCode::Unsupported:
        return vinc::wrap_error(ErrorCode::Unsupported, operation, failure);
    case osal::ErrorCode::Timeout:
        return vinc::wrap_error(ErrorCode::Timeout, operation, failure);
    case osal::ErrorCode::Cancelled:
        return vinc::wrap_error(ErrorCode::Cancelled, operation, failure);
    case osal::ErrorCode::Busy:
        return vinc::wrap_error(ErrorCode::Busy, operation, failure);
    case osal::ErrorCode::SystemError:
        return vinc::wrap_error(ErrorCode::SystemError, operation, failure);
    case osal::ErrorCode::Closed:
        return vinc::wrap_error(ErrorCode::Closed, operation, failure);
    case osal::ErrorCode::WouldBlock:
        return vinc::wrap_error(ErrorCode::WouldBlock, operation, failure);
    case osal::ErrorCode::AlreadyExists:
        return vinc::wrap_error(ErrorCode::AlreadyExists, operation, failure);
    default:
        // Unknown or non-FileLock classifications remain visible in the cause.
        return vinc::wrap_error(ErrorCode::SystemError, operation, failure);
    }
}

namespace {
using State = detail::InterprocessMutexState;

const void* current_thread_token() noexcept {
    // Distinct for live threads, with no registry or per-acquisition allocation.
    // As with thread IDs, the identity is not retained after its thread exits:
    // callers must release every reservation/guard on the acquiring thread.
    thread_local unsigned char token = 0;
    return &token;
}

bool owned_by_current_thread(const State& state) noexcept {
    // This is an identity check only. local_mutex still protects all other state
    // and the token's writes; the token does not publish the protected payload.
    return state.owner_thread_token.load(std::memory_order_relaxed) == current_thread_token();
}

vinc::Error error(ErrorCode code, ErrorOperation operation = ErrorOperation::Lock) {
    return vinc::make_error(code, operation);
}

// Caller holds local_mutex. Release the reservation only after native unlock
// succeeds, or while rolling back an acquisition that never returned a guard.
void release_local_slot(State& state) {
    state.occupied = false;
    state.owner_thread_token.store(nullptr, std::memory_order_relaxed);
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

vinc::Result<void> reserve_local_slot_until(State& state, vinc::Deadline deadline, std::stop_token stop) {
    std::unique_lock lock(state.local_mutex);
    if (stop.stop_requested()) {
        return std::unexpected(error(ErrorCode::Cancelled));
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(error(ErrorCode::Timeout));
    }

    if (state.occupied && owned_by_current_thread(state)) {
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
    state.owner_thread_token.store(current_thread_token(), std::memory_order_relaxed);
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

vinc::Result<InterprocessMutex> InterprocessMutex::create(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::create(path);
    if (!file) {
        return std::unexpected(detail::translate_file_lock_error(file.error(), ErrorOperation::Create));
    }

    state->file_lock = std::move(*file);
    return InterprocessMutex(std::move(state));
}

vinc::Result<InterprocessMutex> InterprocessMutex::open(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::open(path);
    if (!file) {
        return std::unexpected(detail::translate_file_lock_error(file.error(), ErrorOperation::Open));
    }

    state->file_lock = std::move(*file);
    return InterprocessMutex(std::move(state));
}

void InterprocessMutex::close() noexcept {
    state_.reset();
}

vinc::Result<InterprocessMutexGuard> InterprocessMutex::try_lock() const {
    // Retain State for the operation and the guard returned on success.
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    if (owned_by_current_thread(*state)) {
        return std::unexpected(error(ErrorCode::Busy));
    }

    std::unique_lock lock(state->local_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(error(ErrorCode::WouldBlock));
    }

    if (state->occupied) {
        return std::unexpected(error(ErrorCode::WouldBlock));
    }

    // Keep this single bookkeeping lock through the nonwaiting native attempt
    // and publication. With no local reservation outstanding, this private
    // FileLock has no concurrent users of its internal gate. Failure leaves no
    // local slot to roll back or notify, and success needs no mutex reacquisition.
    auto guard = state->file_lock.try_lock();
    if (!guard) {
        return std::unexpected(detail::translate_file_lock_error(guard.error(), ErrorOperation::Lock));
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    state->file_guard.emplace(std::move(*guard));
    state->occupied = true;
    state->owner_thread_token.store(current_thread_token(), std::memory_order_relaxed);
    return InterprocessMutexGuard(std::move(state));
}

vinc::Result<InterprocessMutexGuard> InterprocessMutex::lock_until(vinc::Deadline deadline,
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
        return std::unexpected(detail::translate_file_lock_error(guard.error(), ErrorOperation::Lock));
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

vinc::Result<void> InterprocessMutexGuard::unlock() noexcept {
    if (!state_) {
        return {};
    }

    auto state = state_;
    std::lock_guard lock(state->local_mutex);
    if (!owned_by_current_thread(*state)) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Unlock));
    }

    // Keep protected writes before releasing the native lock.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // Keep local bookkeeping on failure. This alone does not prove the native
    // lock remains held; the failure-state policy is tracked in the README.
    auto result = state->file_guard->unlock();
    if (!result) {
        return std::unexpected(detail::translate_file_lock_error(result.error(), ErrorOperation::Unlock));
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
