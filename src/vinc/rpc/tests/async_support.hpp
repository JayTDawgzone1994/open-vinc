#pragma once
#include <vinc/module/context.hpp>

namespace rpc_test {
// Existing transport regressions need multiple independent in-flight clients.
// Keep their handles alive explicitly; this is test fixture ownership, not an SDK alias.
struct Clients {
    std::vector<std::shared_ptr<void>> retained;
    template<class Request, class Response, class Owner, class Callback>
    vinc::Result<void> send(Owner& owner, std::string target, std::string service,
        const Request& request, vinc::rpc::CallOptions options, Callback callback) {
        auto created = [&]() {
            if constexpr (std::is_same_v<Owner, vinc::Context>) {
                return owner.template rpc_client<Request, Response>(target + "." + service, std::move(callback), options);
            } else {
                return vinc::rpc::detail::RpcClientAccess::create<Request, Response>(
                    vinc::rpc::detail::ClientAccess::endpoint(owner), target + "." + service,
                    std::move(callback), options);
            }
        }();
        if (!created) {
            return std::unexpected(created.error());
        }

        auto client = std::make_shared<vinc::rpc::RpcClient<Request, Response>>(std::move(*created));
        retained.push_back(client);
        return client->call(request);
    }
};
}
