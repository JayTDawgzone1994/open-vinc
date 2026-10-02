#pragma once

// Exclusive ownership of a dedicated local lock file. Ownership belongs to the
// open handle and its guard, NOT the calling thread. No mutex thread policy here.
#include <vinc/osal/error.hpp>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace vinc::osal {
namespace detail {
struct FileLockState;
}

class FileLockGuard {
public:
    FileLockGuard() noexcept;
    // Native unlock failure terminates instead of silently losing ownership.
    // Explicit unlock preserves this guard on error and allows a retry.
    ~FileLockGuard();
    FileLockGuard(FileLockGuard&&) noexcept;
    FileLockGuard& operator=(FileLockGuard&&) noexcept;
    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;
    explicit operator bool() const noexcept;
    // May run on any thread after a synchronized handoff; do not race this guard.
    [[nodiscard]] Result<void> unlock() noexcept;

private:
    friend class FileLock;
    explicit FileLockGuard(std::shared_ptr<detail::FileLockState>) noexcept;
    void release_or_terminate() noexcept;
    std::shared_ptr<detail::FileLockState> state_;
};

class FileLock {
public:
    FileLock() noexcept;
    ~FileLock();
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    explicit operator bool() const noexcept;
    // Absolute, dedicated path in a private local directory. No parent creation,
    // inheritance or implicit unlink. Never replace the path while in use.
    [[nodiscard]] static Result<FileLock> create(const std::filesystem::path&);
    [[nodiscard]] static Result<FileLock> open(const std::filesystem::path&);
    // One pending acquisition/guard per handle. Another operation on the same
    // busy handle returns Busy regardless of thread. Independent open handles
    // contend through the OS (WouldBlock), including within the same process.
    [[nodiscard]] Result<FileLockGuard> try_lock() const;
    [[nodiscard]] Result<FileLockGuard> lock_until(Deadline, std::stop_token = {}) const;
    // A live guard retains the handle. Synchronize close/move/destruction with
    // other calls on this public object; it does not cancel or force-unlock.
    void close() noexcept;

private:
    explicit FileLock(std::shared_ptr<detail::FileLockState>) noexcept;
    std::shared_ptr<detail::FileLockState> state_;
};
} // namespace vinc::osal
