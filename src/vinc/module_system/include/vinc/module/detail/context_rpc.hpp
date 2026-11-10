#pragma once
#include <vinc/module/context.hpp>

namespace vinc::detail {
// Language adapters use the same admitted Context and transport, never a second client.
struct ContextRpcAccess {
    static Result<rpc::detail::Address> prepare(Context& context, std::string_view address) {
        return context.prepare_rpc_call(address);
    }
    static rpc::Client& client(Context& context) {
        return context.rpc_;
    }
};
}
