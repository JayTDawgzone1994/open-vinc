#pragma once
#include <vinc/module/context.hpp>

namespace vinc::detail {
// Host-owned wiring, valid through Module destruction. No Plugin owns a receiver
// or control service. Calls into application code are made without the inbox lock.
class ContextRuntime {
public:
    virtual ~ContextRuntime() = default;
    virtual bool living() const noexcept = 0;
    virtual Result<std::uint64_t> commands() = 0;
    virtual void wait() = 0;
    virtual void fail(const Error&) = 0;
};
}
