// Portable exclusive lock-file handle lifetime and cancellable acquisition.
// Native backends alone know how a file lock is represented by the OS.
#include "file_lock_state.hpp"
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <utility>

namespace vinc::osal {
namespace {
using State = detail::FileLockState;

// Retry native nonblocking acquisition while keeping deadline/cancellation checks responsive.
constexpr auto lock_retry_interval = std::chrono::milliseconds(5);

OsError error(ErrorCode code, Operation operation = Operation::LockFileLock) {
    return {code, operation};
}

bool valid_path(const std::filesystem::path& path) {
    const auto& value = path.native();
    return path.is_absolute() && path.has_filename() && path.filename() != "." && path.filename() != ".." &&
           value.find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
}

// Claim this handle, not a thread. A second guard cannot accidentally unlock an
// earlier guard (flock may otherwise report repeated locking as success).
struct Pending {
    State& state;
    bool acquired = false;

    ~Pending() {
        if (!acquired) {
            std::lock_guard lock(state.gate);
            state.occupied = false;
        }
    }
};

Result<void> reserve(State& state) {
    std::lock_guard lock(state.gate);
    if (state.occupied) {
        return std::unexpected(error(ErrorCode::Busy));
    }
    state.occupied = true;
    return {};
}
}

FileLock::FileLock() noexcept = default;
FileLock::~FileLock() = default;
FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;

FileLock::FileLock(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {
}

FileLock::operator bool() const noexcept {
    return bool(state_);
}

Result<FileLock> FileLock::create(const std::filesystem::path& path) {
    if (!valid_path(path)) {
        return std::unexpected(error(ErrorCode::InvalidArgument, Operation::CreateFileLock));
    }

    auto state = std::make_shared<State>();
    auto result = detail::open_file_lock(*state, path, true);
    if (!result) {
        return std::unexpected(result.error());
    }

    return FileLock(std::move(state));
}

Result<FileLock> FileLock::open(const std::filesystem::path& path) {
    if (!valid_path(path)) {
        return std::unexpected(error(ErrorCode::InvalidArgument, Operation::OpenFileLock));
    }

    auto state = std::make_shared<State>();
    auto result = detail::open_file_lock(*state, path, false);
    if (!result) {
        return std::unexpected(result.error());
    }

    return FileLock(std::move(state));
}

void FileLock::close() noexcept {
    state_.reset();
}

Result<FileLockGuard> FileLock::try_lock() const {
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    auto reserved = reserve(*state);
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    // Roll back the handle reservation if native acquisition fails.
    Pending pending{*state};
    auto result = detail::try_lock_file_lock(*state);
    if (!result) {
        return std::unexpected(result.error());
    }
    pending.acquired = true;
    return FileLockGuard(std::move(state));
}

Result<FileLockGuard> FileLock::lock_until(Deadline deadline, std::stop_token stop) const {
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    if (stop.stop_requested()) {
        return std::unexpected(error(ErrorCode::Cancelled));
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(error(ErrorCode::Timeout));
    }

    auto reserved = reserve(*state);
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    // Roll back the handle reservation if native acquisition fails.
    Pending pending{*state};
    std::mutex wait_mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(wait_mutex);
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(error(ErrorCode::Cancelled));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(error(ErrorCode::Timeout));
        }

        auto result = detail::try_lock_file_lock(*state);
        if (result) {
            pending.acquired = true;
            return FileLockGuard(std::move(state));
        }

        if (result.error().code != ErrorCode::WouldBlock) {
            return std::unexpected(result.error());
        }
        wake.wait_until(lock, stop, (std::min)(deadline, now + lock_retry_interval), [] {
            return false;
        });
    }
}

FileLockGuard::FileLockGuard() noexcept = default;

FileLockGuard::FileLockGuard(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {
}

FileLockGuard::~FileLockGuard() {
    release_or_terminate();
}

FileLockGuard::FileLockGuard(FileLockGuard&& other) noexcept : state_(std::move(other.state_)) {
}

FileLockGuard& FileLockGuard::operator=(FileLockGuard&& other) noexcept {
    if (this != &other) {
        release_or_terminate();
        state_ = std::move(other.state_);
    }

    return *this;
}

FileLockGuard::operator bool() const noexcept {
    return bool(state_);
}

Result<void> FileLockGuard::unlock() noexcept {
    if (!state_) {
        return {};
    }

    auto state = state_;
    std::lock_guard lock(state->gate);
    auto result = detail::unlock_file_lock(*state);
    if (!result) {
        return result;
    }
    // Release the handle reservation only after the OS confirms unlock.
    state->occupied = false;
    state_.reset();
    return {};
}

void FileLockGuard::release_or_terminate() noexcept {
    if (!unlock()) {
        std::terminate();
    }
}
} // namespace vinc::osal
