#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/concurrency/detail/mutex_recovery.hpp>
#include <vinc/osal/shared_memory.hpp>

namespace vinc::detail {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
Error failed(const Error& cause, ErrorOperation operation = ErrorOperation::AccessLock) {
    return wrap_error(ErrorCode::LockFailed, operation, cause);
}
}

LockRegistry::LockRegistry(std::shared_ptr<ResourceNames> resources) : resources_(std::move(resources)) {
}

LockRegistry::~LockRegistry() {
    for (auto& entry : entries_) {
        if (!entry) {
            continue;
        }
        entry->recovery.close();
        try {
            auto unlinked = osal::SharedMemory::unlink_name(entry->descriptor.name);
            if (unlinked || unlinked.error().is(osal::ErrorCode::NotFound) ||
                unlinked.error().is(osal::ErrorCode::Unsupported)) {
                (void)resources_->retire(entry->descriptor.name);
            }
        } catch (...) {
            // The journal keeps incomplete cleanup for the next admitted central.
        }
    }
}

Result<LockDescriptor> LockRegistry::create() {
    std::lock_guard creation_lock(creation_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (count_ == entries_.size()) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::AccessLock});
        }
    }
    auto entry = std::make_shared<Entry>();
    auto name = resources_->reserve();
    if (!name) {
        return std::unexpected(failed(name.error()));
    }
    entry->descriptor.name = std::move(*name);
    auto created = concurrency::InterprocessMutex::create(entry->descriptor.name, central_owner);
    if (!created) {
        return std::unexpected(failed(created.error()));
    }
    entry->recovery = std::move(*created);
    // Disk/native acquisition runs outside this short local registry lock.
    std::lock_guard lock(mutex_);
    entry->descriptor.id = count_ + 1;
    entries_[count_++] = entry;
    return entry->descriptor;
}

Result<LockDescriptor> LockRegistry::descriptor(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    if (!id || id > count_) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::AccessLock});
    }
    return entries_[id - 1]->descriptor;
}

Result<void> LockRegistry::exited(MID mid) {
    if (!valid_mid(mid)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::RecoverLock});
    }
    std::array<std::shared_ptr<Entry>, config::module_lock_limit> entries;
    {
        std::lock_guard lock(mutex_);
        entries = entries_;
    }
    // Bounded by module_lock_limit; no waiting, allocation, payload repair or callbacks.
    for (const auto& entry : entries) {
        if (!entry) {
            break;
        }
        auto held = concurrency::detail::MutexRecovery::acquire_after_exit(entry->recovery, mid);
        if (!held) {
            if (held.error().is(concurrency::ErrorCode::WouldBlock)) {
                continue; // No longer owned by this dead MID. Never steal another owner.
            }
            return std::unexpected(failed(held.error(), ErrorOperation::RecoverLock));
        }
        auto released = held->unlock();
        if (!released) {
            return std::unexpected(failed(released.error(), ErrorOperation::RecoverLock));
        }
    }
    return {};
}
}
