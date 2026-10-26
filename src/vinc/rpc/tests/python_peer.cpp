#include <vinc/module/base_module.hpp>
#include <vinc_messages/int3.hpp>
#include <stdexcept>

namespace {
using namespace vinc;

class RpcPeer final : public BaseModule<RunMode::FrameworkLoop> {
    bool returned_ = false;

public:
    RpcPeer() : BaseModule("rpcNative") {
    }

    Result<void> on_start(const std::vector<std::string>&) override {
        return context.register_rpc<int3, int3>("echo", {}, [this](const int3& request, int3& response) {
            response.data().x = request.data().x.get() + 1;
            auto sent = context.call_rpc<int3, int3>(
                "rpcPython", "echo", request, {}, [this](Result<rpc::RpcReply<int3>> reply) {
                    if (!reply || reply->response->data().x.get() != 44) {
                        throw std::runtime_error("Python response failed");
                    }

                    returned_ = true;
                });
            if (!sent) {
                throw std::runtime_error(error_to_string(sent.error()));
            }

            return rpc::RpcStatus::success();
        });
    }

    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return returned_ ? 1 : 0;
    }
};
}

REGISTER_MODULE(RpcPeer, "rpcNativeFixture")
