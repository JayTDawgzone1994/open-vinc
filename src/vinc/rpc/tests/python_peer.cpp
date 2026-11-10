#include <vinc/module/base_module.hpp>
#include <vinc_messages/int3.hpp>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;

class RpcPeer final : public BaseModule<RunMode::FrameworkLoop> {
    bool returned_ = false;
    bool sent_ = false;
    rpc::RpcClient<int3, int3> client_;

public:
    RpcPeer() : BaseModule("rpcNative") {
    }

    Result<void> on_start(const std::vector<std::string>&) override {
        auto created = context.rpc_client<int3, int3>("rpcPython.echo", [this](Result<rpc::RpcReply<int3>> reply) {
            if (!reply || reply->response->data().x.get() != 44) {
                throw std::runtime_error("Python response failed");
            }

            returned_ = true;
        });
        if (!created) {
            return std::unexpected(created.error());
        }

        client_ = std::move(*created);

        auto sync = context.register_rpc<int3, int3>("sync", [](const int3& request, int3& response) {
            constexpr auto gil_observation_delay = std::chrono::milliseconds(100);
            std::this_thread::sleep_for(gil_observation_delay);
            response.data().x = request.data().x.get() + 2;
            return rpc::RpcStatus::success();
        });
        if (!sync) {
            return sync;
        }

        return context.register_rpc<int3, int3>("echo", [this](const int3& request, int3& response) {
            response.data().x = request.data().x.get() + 1;
            if (!sent_) {
                auto sent = client_.call(request);
                if (!sent) {
                    throw std::runtime_error(error_to_string(sent.error()));
                }

                sent_ = true;
            }

            return rpc::RpcStatus::success();
        }, {});
    }

    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return returned_ ? 1 : 0;
    }
};
}

REGISTER_MODULE(RpcPeer, "rpcNativeFixture")
