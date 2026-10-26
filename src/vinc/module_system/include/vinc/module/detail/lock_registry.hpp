#pragma once
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/module/detail/lock_descriptor.hpp>
#include <vinc/detail/resource_names.hpp>
#include <vinc/module/error.hpp>
#include <vinc/module/mid.hpp>
#include <vinc/config.hpp>
#include <array>
#include <mutex>

namespace vinc::detail {
// Central integration only. Registered locks live until registry destruction.
// Recovery handles are never used for ordinary central payload operations.
class LockRegistry {
public:
    explicit LockRegistry(std::shared_ptr<ResourceNames> resources);
    ~LockRegistry();
    Result<LockDescriptor> create();
    Result<LockDescriptor> descriptor(std::uint64_t id);
    Result<void> exited(MID mid); // OS-confirmed process exit is a precondition.
private:
    struct Entry {
        LockDescriptor descriptor;
        concurrency::InterprocessMutex recovery;
    };
    std::shared_ptr<ResourceNames> resources_;
    std::mutex creation_mutex_; // Serializes native creation, never taken by recovery.
    std::mutex mutex_;
    std::array<std::shared_ptr<Entry>, config::module_lock_limit> entries_{};
    std::size_t count_ = 0;
};
}
