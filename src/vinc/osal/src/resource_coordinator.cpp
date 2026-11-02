#include <vinc/osal/detail/resource_coordinator.hpp>
#include <atomic>
#include <utility>

namespace vinc::osal::detail {
namespace {
enum class Selection { Unused, Binding, Borrowed, Default };
constinit std::atomic<Selection> selection{Selection::Unused};
ResourceCoordination borrowed;

ResourceCoordinator& default_owner() {
    // Intentionally process-lifetime storage: static destructors may still use
    // OSAL. Suppress destruction, without heap allocation or a native FD leak.
    union Lifetime {
        ResourceCoordinator owner;
        Lifetime() : owner{} {}
        ~Lifetime() {}
    };
    static Lifetime lifetime;
    return lifetime.owner;
}
}

ResourceCoordination ResourceCoordinator::services() noexcept {
    return {resource_coordinator_version, sizeof(ResourceCoordination), this, acquire, release};
}

Result<void> ResourceCoordinator::acquire(void* context, bool try_only, Deadline deadline,
                                         const std::stop_token& stop) {
    auto& owner = *static_cast<ResourceCoordinator*>(context);
    if (try_only) {
        std::unique_lock lock(owner.mutex_, std::try_to_lock);
        if (!lock.owns_lock() || owner.held_) {
            return std::unexpected(Error{ErrorCode::WouldBlock, ErrorOperation::AcquireResources});
        }

        owner.held_ = true;
        return {};
    }

    // Register before taking mutex_: a pre-cancelled callback runs inline.
    // Its matching mutex prevents a cancellation wake from being lost between
    // the predicate check and wait. lock dies before callback deregistration.
    std::stop_callback cancelled(stop, [&owner] {
        std::lock_guard lock(owner.mutex_);
        owner.changed_.notify_all();
    });
    std::unique_lock lock(owner.mutex_);
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::AcquireResources});
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::AcquireResources});
        }
        if (!owner.held_) {
            owner.held_ = true;
            return {};
        }

        owner.changed_.wait_until(lock, deadline);
    }
}

void ResourceCoordinator::release(void* context) noexcept {
    auto& owner = *static_cast<ResourceCoordinator*>(context);
    std::lock_guard lock(owner.mutex_);
    owner.held_ = false;
    // A single chosen waiter may expire/cancel rather than take ownership.
    // Wake all predicates so an indefinite waiter cannot miss the free slot.
    owner.changed_.notify_all();
}

Result<ResourceCoordination> resource_coordination() {
    auto phase = Selection::Unused;
    if (selection.compare_exchange_strong(phase, Selection::Default, std::memory_order_acq_rel)) {
        return default_owner().services();
    }
    if (phase == Selection::Borrowed) {
        return borrowed;
    }
    if (phase == Selection::Default) {
        return default_owner().services();
    }

    // Initialization must finish before application operations. Do not spin or
    // run on the default while another thread publishes a borrowed owner.
    return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::AcquireResources});
}

Result<void> bind_resource_coordination(const ResourceCoordination& services) noexcept {
    if (services.version != resource_coordinator_version) {
        return std::unexpected(Error{ErrorCode::IncompatibleCoordinator, ErrorOperation::BindCoordinator});
    }
    if (services.size < sizeof(ResourceCoordination) || !services.context ||
        !services.acquire || !services.release) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::BindCoordinator});
    }

    auto phase = Selection::Unused;
    if (!selection.compare_exchange_strong(phase, Selection::Binding, std::memory_order_acq_rel)) {
        const auto code = phase == Selection::Default ? ErrorCode::CoordinatorInUse
                                                      : ErrorCode::CoordinatorAlreadyBound;
        return std::unexpected(Error{code, ErrorOperation::BindCoordinator});
    }

    borrowed = services;
    selection.store(Selection::Borrowed, std::memory_order_release);
    return {};
}

ResourceGuard::~ResourceGuard() {
    if (services_.context) {
        services_.release(services_.context);
    }
}

ResourceGuard::ResourceGuard(ResourceGuard&& other) noexcept : services_(other.services_) {
    other.services_.context = nullptr;
}

Result<ResourceGuard> ResourceGuard::acquire(bool try_only, Deadline deadline, const std::stop_token& stop) {
    auto services = resource_coordination();
    if (!services) {
        return std::unexpected(services.error());
    }

    auto acquired = services->acquire(services->context, try_only, deadline, stop);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }

    return ResourceGuard(*services);
}

Result<ResourceGuard> ResourceGuard::try_acquire() {
    return acquire(true, Deadline::max(), {});
}

Result<ResourceGuard> ResourceGuard::acquire_until(Deadline deadline, std::stop_token stop) {
    return acquire(false, deadline, stop);
}
}
