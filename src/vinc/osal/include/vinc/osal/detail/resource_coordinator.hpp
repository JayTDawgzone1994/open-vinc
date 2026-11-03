#pragma once

#include <vinc/detail/local.hpp>
#include <vinc/osal/error.hpp>
#include <vinc/time.hpp>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace vinc::osal::detail {
inline constexpr std::uint32_t resource_coordinator_version = 1;

// Same-process, matching-build ABI. Only the owner operates/destroys its mutex.
// Tables are copied; context and callback code must outlive all borrowers,
// including library unloading destructors. This is not an IPC representation.
struct ResourceCoordination {
    std::uint32_t version = resource_coordinator_version;
    std::uint32_t size = sizeof(ResourceCoordination);
    void* context = nullptr;
    Result<void> (*acquire)(void*, bool, Deadline, const std::stop_token&) = nullptr;
    void (*release)(void*) noexcept = nullptr;
};

// A non-recursive owner. No native descriptors, worker, registry, or heap-owned
// synchronization object. Destroy only after all guards/waiters/borrowers finish.
class VINC_LOCAL ResourceCoordinator {
public:
    ResourceCoordination services() noexcept;

private:
    static Result<void> acquire(void* context, bool try_only, Deadline deadline,
                                const std::stop_token& stop);
    static void release(void* context) noexcept;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool held_ = false;
};

// Selects this image's coordinator permanently, even on an unsuccessful attempt
// to acquire it. Standalone defaults have process lifetime. Binding must precede
// any call to this function; no unbind/private fallback is provided.
VINC_LOCAL Result<ResourceCoordination> resource_coordination();
VINC_LOCAL Result<void> bind_resource_coordination(const ResourceCoordination& services) noexcept;

// Hold only around native acquisition/relocation/flags/rollback or stdio snapshot.
// Never hold across readiness waits, spawn, callbacks, DNS, loader, or ordinary I/O.
// The image-selection phase completes before the owner's mutex is entered.
class VINC_LOCAL ResourceGuard {
public:
    ~ResourceGuard();
    ResourceGuard(ResourceGuard&& other) noexcept;
    ResourceGuard(const ResourceGuard&) = delete;
    ResourceGuard& operator=(const ResourceGuard&) = delete;

    static Result<ResourceGuard> try_acquire();
    static Result<ResourceGuard> acquire_until(Deadline deadline, std::stop_token stop = {});

private:
    explicit ResourceGuard(ResourceCoordination services) noexcept : services_(services) {}
    static Result<ResourceGuard> acquire(bool try_only, Deadline deadline, const std::stop_token& stop);
    ResourceCoordination services_;
};
}
