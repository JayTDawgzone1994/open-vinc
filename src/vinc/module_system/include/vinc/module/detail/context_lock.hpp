#pragma once
#include <vinc/result.hpp>
#include <vinc/time.hpp>
#include <memory>

namespace vinc {
class Context;
namespace detail {
// Host-owned lease: its virtual operations and destruction stay in the Host.
// Only framework service implementations may use this integration surface.
class ContextLock {
public:
    virtual ~ContextLock() = default;
    virtual bool publication() const noexcept = 0;
    virtual Result<void> unlock() noexcept = 0;
    virtual Result<void> publish_and_unlock() noexcept = 0;
};

struct ContextLockAccess {
    static Result<std::unique_ptr<ContextLock>> lock(Context& context, std::uint64_t id,
                                                   Deadline deadline);
};
}
}
