#pragma once

#include <vinc/time.hpp>
#include <vinc/owner_id.hpp>
#include <vinc/concurrency/error.hpp>
#include <memory>
#include <stop_token>
#include <string_view>

namespace vinc::concurrency {
namespace detail {
struct InterprocessMutexState;
struct MutexRecovery;
}

class InterprocessMutexGuard {
public:
    InterprocessMutexGuard() noexcept = default;
    ~InterprocessMutexGuard();
    InterprocessMutexGuard(InterprocessMutexGuard&& other) noexcept;
    InterprocessMutexGuard& operator=(InterprocessMutexGuard&& other) noexcept;
    InterprocessMutexGuard(const InterprocessMutexGuard&) = delete;
    InterprocessMutexGuard& operator=(const InterprocessMutexGuard&) = delete;
    explicit operator bool() const noexcept;
    // Only while held. The bit is opaque here; all payload readers must copy
    // under this mutex. Mapping lifetime is retained by the guard.
    bool publication() const noexcept;
    bool recovered() const noexcept;
    // Ordinary release/destruction preserves publication (aborts an A/B update).
    // Must run on the acquiring thread. Repeated unlock is harmless.
    [[nodiscard]] Result<void> unlock() noexcept;
    // Bit flip and owner release have ONE atomic linearization point.
    [[nodiscard]] Result<void> publish_and_unlock() noexcept;

private:
    friend class InterprocessMutex;
    friend struct detail::MutexRecovery;
    InterprocessMutexGuard(std::shared_ptr<detail::InterprocessMutexState> state,
                           std::uint64_t held_word, bool recovered = false) noexcept;
    Result<void> release(bool publish) noexcept;
    void release_or_violate() noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
    std::uint64_t held_word_ = 0;
    bool recovered_ = false;
};

class InterprocessMutex {
public:
    InterprocessMutex() noexcept = default;
    ~InterprocessMutex() = default;
    InterprocessMutex(InterprocessMutex&&) noexcept = default;
    InterprocessMutex& operator=(InterprocessMutex&&) noexcept = default;
    InterprocessMutex(const InterprocessMutex&) = delete;
    InterprocessMutex& operator=(const InterprocessMutex&) = delete;
    explicit operator bool() const noexcept;

    // OSAL SharedMemory names. One process always uses the same supervisor-issued
    // owner in this epoch. Names/owners are never reused while peers can exist.
    // Publish the name only after create returns. Open never initializes memory.
    [[nodiscard]] static Result<InterprocessMutex> create(std::string_view name, OwnerId owner);
    [[nodiscard]] static Result<InterprocessMutex> open(std::string_view name, OwnerId owner);
    [[nodiscard]] Result<InterprocessMutexGuard> try_lock() const;
    [[nodiscard]] Result<InterprocessMutexGuard> lock_until(Deadline deadline,
                                                         std::stop_token stop = {}) const;
    // Diagnostic snapshot, not proof of liveness or authority to reclaim.
    [[nodiscard]] Result<OwnerId> owner() const noexcept;
    // Detaches this handle; guards/waits retain the mapping. Close/move must be
    // synchronized with calls on this public object. Does not unlink the name.
    void close() noexcept;

private:
    friend struct detail::MutexRecovery;
    explicit InterprocessMutex(std::shared_ptr<detail::InterprocessMutexState> state) noexcept;
    std::shared_ptr<detail::InterprocessMutexState> state_;
};
}
