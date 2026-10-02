// Thread-owned mutex policy composed from OSAL FileLock. No native OS handles,
// platform headers, conditional backends, or Param/Module knowledge here.
#include <vinc/sync/interprocess_mutex.hpp>
#include <vinc/osal/file_lock.hpp>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::sync {
namespace detail {
struct InterprocessMutexState {
    osal::FileLock file;
    std::mutex gate;
    std::condition_variable_any changed;
    // Reserve the local thread slot before blocking on FileLock. Other local
    // threads wait here instead of concurrently acquiring the same file handle.
    bool occupied = false;
    std::thread::id owner;
    std::optional<osal::FileLockGuard> guard;
};
}
namespace {
using State = detail::InterprocessMutexState;
Error error(ErrorCode code, Operation operation = Operation::Lock) { return {code, operation, {}}; }
Error native_error(const osal::OsError& e, Operation operation) { return {e.code, operation, e}; }
void release_slot(State& state) {
    state.occupied = false; state.owner = {}; state.changed.notify_all();
}
// On failed acquisition or a C++ exception, never leave a local slot reserved.
struct Pending {
    State& state;
    bool acquired = false;
    ~Pending() { if (!acquired) { std::lock_guard lock(state.gate); release_slot(state); } }
};
}
InterprocessMutex::InterprocessMutex() noexcept = default;
InterprocessMutex::~InterprocessMutex() = default;
InterprocessMutex::InterprocessMutex(InterprocessMutex&&) noexcept = default;
InterprocessMutex& InterprocessMutex::operator=(InterprocessMutex&&) noexcept = default;
InterprocessMutex::InterprocessMutex(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
InterprocessMutex::operator bool() const noexcept { return bool(state_); }
Result<InterprocessMutex> InterprocessMutex::create(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::create(path);
    if (!file) return std::unexpected(native_error(file.error(), Operation::Create));
    state->file = std::move(*file); return InterprocessMutex(std::move(state));
}
Result<InterprocessMutex> InterprocessMutex::open(const std::filesystem::path& path) {
    auto state = std::make_shared<State>();
    auto file = osal::FileLock::open(path);
    if (!file) return std::unexpected(native_error(file.error(), Operation::Open));
    state->file = std::move(*file); return InterprocessMutex(std::move(state));
}
void InterprocessMutex::close() noexcept { state_.reset(); }
Result<InterprocessMutexGuard> InterprocessMutex::try_lock() const {
    auto state = state_;
    if (!state) return std::unexpected(error(ErrorCode::Closed));
    {
        std::lock_guard lock(state->gate);
        if (state->occupied) return std::unexpected(error(state->owner == std::this_thread::get_id() ? ErrorCode::Busy : ErrorCode::WouldBlock));
        state->occupied = true; state->owner = std::this_thread::get_id();
    }
    Pending pending{*state};
    auto guard = state->file.try_lock();
    if (!guard) return std::unexpected(native_error(guard.error(), Operation::Lock));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    { std::lock_guard lock(state->gate); state->guard.emplace(std::move(*guard)); }
    pending.acquired = true; return InterprocessMutexGuard(std::move(state));
}
Result<InterprocessMutexGuard> InterprocessMutex::lock_until(osal::Deadline deadline, std::stop_token stop) const {
    auto state = state_;
    if (!state) return std::unexpected(error(ErrorCode::Closed));
    {
        std::unique_lock lock(state->gate);
        if (stop.stop_requested()) return std::unexpected(error(ErrorCode::Cancelled));
        if (std::chrono::steady_clock::now() >= deadline) return std::unexpected(error(ErrorCode::Timeout));
        if (state->occupied && state->owner == std::this_thread::get_id()) return std::unexpected(error(ErrorCode::Busy));
        state->changed.wait_until(lock, stop, deadline, [&] { return !state->occupied; });
        if (stop.stop_requested()) return std::unexpected(error(ErrorCode::Cancelled));
        if (std::chrono::steady_clock::now() >= deadline) return std::unexpected(error(ErrorCode::Timeout));
        state->occupied = true; state->owner = std::this_thread::get_id();
    }
    Pending pending{*state};
    // One original deadline/token covers BOTH local and cross-process waits.
    auto guard = state->file.lock_until(deadline, stop);
    if (!guard) return std::unexpected(native_error(guard.error(), Operation::Lock));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    { std::lock_guard lock(state->gate); state->guard.emplace(std::move(*guard)); }
    pending.acquired = true; return InterprocessMutexGuard(std::move(state));
}
InterprocessMutexGuard::InterprocessMutexGuard() noexcept = default;
InterprocessMutexGuard::InterprocessMutexGuard(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
InterprocessMutexGuard::~InterprocessMutexGuard() { release_or_terminate(); }
InterprocessMutexGuard::InterprocessMutexGuard(InterprocessMutexGuard&& other) noexcept : state_(std::move(other.state_)) {}
InterprocessMutexGuard& InterprocessMutexGuard::operator=(InterprocessMutexGuard&& other) noexcept {
    if (this != &other) { release_or_terminate(); state_ = std::move(other.state_); }
    return *this;
}
InterprocessMutexGuard::operator bool() const noexcept { return bool(state_); }
Result<void> InterprocessMutexGuard::unlock() noexcept {
    if (!state_) return {};
    auto state = state_;
    std::lock_guard lock(state->gate);
    if (state->owner != std::this_thread::get_id()) return std::unexpected(error(ErrorCode::InvalidArgument, Operation::Unlock));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto result = state->guard->unlock();
    if (!result) return std::unexpected(native_error(result.error(), Operation::Unlock));
    state->guard.reset(); release_slot(*state); state_.reset(); return {};
}
void InterprocessMutexGuard::release_or_terminate() noexcept { if (!unlock()) std::terminate(); }
} // namespace vinc::sync
