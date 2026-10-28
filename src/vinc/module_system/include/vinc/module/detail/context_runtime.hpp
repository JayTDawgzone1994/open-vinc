#pragma once
#include <vinc/module/context.hpp>
#include <vinc/module/detail/context_lock.hpp>

namespace vinc::detail {
// Host-owned wiring, valid through Module destruction. No Plugin owns a receiver
// or control service. Calls into application code are made without the inbox lock.
class ContextRuntime {
public:
    virtual ~ContextRuntime() = default;
    virtual bool living() const noexcept = 0;
    virtual Result<ModulePage> list_modules(MID after, Deadline deadline) = 0;
    virtual Result<std::uint64_t> commands() = 0;
    virtual void wait() = 0;
    virtual void wake() = 0;
    virtual void fail(const Error&) = 0;
    virtual Result<std::unique_ptr<ContextLock>> lock(std::uint64_t id, Deadline deadline) = 0;
};

// Language adapters report escaped callbacks without introducing an exception
// into the C++ callback contract. Called only while the Context is admitted.
struct ContextLanguageAccess {
    static void fail(Context& context, const Error& error) {
        if (context.runtime_) {
            context.runtime_->fail(error);
        }
    }
};
}
