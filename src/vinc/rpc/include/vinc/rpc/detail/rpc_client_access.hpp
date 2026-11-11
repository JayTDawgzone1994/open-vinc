#pragma once
#include <vinc/rpc/rpc_client.hpp>

namespace vinc::rpc::detail {
// Designated Context/language integration only. Applications obtain handles
// from Context; a custom endpoint must not bypass authenticated Module identity.
struct RpcClientAccess {
    template <Message Request, Message Response, class Completion>
        requires std::invocable<Completion&, Result<RpcReply<Response>>>
    static Result<RpcClient<Request, Response>> create(std::weak_ptr<CallEndpoint> endpoint,
                                                       std::string_view address, Completion completion,
                                                       CallOptions options = {}) {
        static_assert(std::is_same_v<std::invoke_result_t<Completion&, Result<RpcReply<Response>>>, void>,
                      "RPC completion callbacks return void");
        return RpcClient<Request, Response>::create(
            std::move(endpoint), address, std::move(completion), options);
    }
};
}
