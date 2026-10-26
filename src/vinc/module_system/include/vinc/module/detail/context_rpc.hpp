#pragma once
#include <vinc/module/context.hpp>

namespace vinc::detail {
// Language adapters use the same admitted Context and transport, never a second client.
struct ContextRpcAccess {
    static rpc::Client& client(Context& context) {
        return context.rpc_;
    }
};
}
