#pragma once

// Framework synchronization built exclusively on the OSAL FileLock abstraction.
// A dedicated local lock file identifies cooperating processes' exclusive lock.
#include <vinc/sync/error.hpp>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace vinc::sync {
namespace detail { struct InterprocessMutexState; }

class InterprocessMutexGuard {
public:
    InterprocessMutexGuard() noexcept;
    // A failed unlock (including wrong-thread destruction) terminates rather
    // than silently discarding ownership. Use explicit unlock to handle errors.
    ~InterprocessMutexGuard();
    InterprocessMutexGuard(InterprocessMutexGuard&&) noexcept;
    InterprocessMutexGuard& operator=(InterprocessMutexGuard&&) noexcept;
    InterprocessMutexGuard(const InterprocessMutexGuard&) = delete;
    InterprocessMutexGuard& operator=(const InterprocessMutexGuard&) = delete;
    explicit operator bool() const noexcept;
    // Idempotent after success. Must run on the acquiring thread. Native failure
    // leaves ownership intact for retry; wrong-thread use returns InvalidArgument.
    [[nodiscard]] Result<void> unlock() noexcept;
private:
    friend class InterprocessMutex;
    explicit InterprocessMutexGuard(std::shared_ptr<detail::InterprocessMutexState>) noexcept;
    void release_or_terminate() noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
};

class InterprocessMutex {
public:
    InterprocessMutex() noexcept;
    ~InterprocessMutex();
    InterprocessMutex(InterprocessMutex&&) noexcept;
    InterprocessMutex& operator=(InterprocessMutex&&) noexcept;
    InterprocessMutex(const InterprocessMutex&) = delete;
    InterprocessMutex& operator=(const InterprocessMutex&) = delete;
    explicit operator bool() const noexcept;

    // Absolute path in an existing private directory on a local filesystem.
    // create is exclusive; open never creates. No implicit parent directories,
    // inheritance, path replacement or unlink on destruction. Caller must not
    // remove/replace/reuse the path until ALL participants have closed it.
    [[nodiscard]] static Result<InterprocessMutex> create(const std::filesystem::path&);
    [[nodiscard]] static Result<InterprocessMutex> open(const std::filesystem::path&);
    // Concurrent acquisition is supported. Recursive use through this handle
    // reports Busy; contention reports WouldBlock. Separately opened handles
    // also contend through the OS, even when owned by the same process.
    [[nodiscard]] Result<InterprocessMutexGuard> try_lock() const;
    [[nodiscard]] Result<InterprocessMutexGuard> lock_until(osal::Deadline, std::stop_token = {}) const;
    // Detach this handle; a guard or already-running wait retains the resource.
    // close/move/destruction must not race calls on this public object.
    void close() noexcept;
private:
    explicit InterprocessMutex(std::shared_ptr<detail::InterprocessMutexState>) noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
};
} // namespace vinc::sync
