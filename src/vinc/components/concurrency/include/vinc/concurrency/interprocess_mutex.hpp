#pragma once

// Framework synchronization built exclusively on the OSAL FileLock abstraction.
// A dedicated local lock file identifies cooperating processes' exclusive lock.
#include <vinc/time.hpp>
#include <vinc/concurrency/error.hpp>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace vinc::concurrency {
namespace detail {
struct InterprocessMutexState;
struct InterprocessMutexTestAccess;
}

class InterprocessMutexGuard {
public:
    InterprocessMutexGuard() noexcept;
    // A failed unlock (including wrong-thread destruction) terminates rather
    // than silently discarding ownership. Use explicit unlock to handle errors.
    ~InterprocessMutexGuard();
    InterprocessMutexGuard(InterprocessMutexGuard&& other) noexcept;
    InterprocessMutexGuard& operator=(InterprocessMutexGuard&& other) noexcept;
    InterprocessMutexGuard(const InterprocessMutexGuard&) = delete;
    InterprocessMutexGuard& operator=(const InterprocessMutexGuard&) = delete;
    explicit operator bool() const noexcept;
    // Idempotent after success. Must run on the acquiring thread. Native failure
    // retains the guard/local reservation; native lock state is not guaranteed
    // for every failure (see README). Wrong-thread use returns InvalidArgument.
    [[nodiscard]] vinc::Result<void> unlock() noexcept;

private:
    friend class InterprocessMutex;
    explicit InterprocessMutexGuard(std::shared_ptr<detail::InterprocessMutexState> state) noexcept;
    void release_or_terminate() noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
};

class InterprocessMutex {
public:
    InterprocessMutex() noexcept;
    ~InterprocessMutex();
    InterprocessMutex(InterprocessMutex&& other) noexcept;
    InterprocessMutex& operator=(InterprocessMutex&& other) noexcept;
    InterprocessMutex(const InterprocessMutex&) = delete;
    InterprocessMutex& operator=(const InterprocessMutex&) = delete;
    explicit operator bool() const noexcept;

    // Absolute path in an existing private directory on a local filesystem.
    // create is exclusive; open never creates. No implicit parent directories,
    // inheritance, path replacement or unlink on destruction. Caller must not
    // remove/replace/reuse the path until ALL participants have closed it.
    [[nodiscard]] static vinc::Result<InterprocessMutex> create(const std::filesystem::path& path);
    [[nodiscard]] static vinc::Result<InterprocessMutex> open(const std::filesystem::path& path);
    // Concurrent acquisition is supported. Recursive use through this handle
    // reports Busy, even during bookkeeping contention. try_lock does not wait
    // for local bookkeeping or another owner: either contention returns WouldBlock.
    // Separately opened handles also contend through the OS in the same process.
    [[nodiscard]] vinc::Result<InterprocessMutexGuard> try_lock() const;
    [[nodiscard]] vinc::Result<InterprocessMutexGuard> lock_until(vinc::Deadline deadline,
                                                            std::stop_token stop = {}) const;
    // Detach this handle; a guard or already-running wait retains the resource.
    // close/move/destruction must not race calls on this public object.
    void close() noexcept;

private:
    // Defined only in tests; no runtime hook or public access to bookkeeping.
    friend struct detail::InterprocessMutexTestAccess;
    explicit InterprocessMutex(std::shared_ptr<detail::InterprocessMutexState> state) noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
};
} // namespace vinc::concurrency
