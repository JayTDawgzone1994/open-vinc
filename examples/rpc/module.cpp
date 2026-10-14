#include <vinc/vinc.hpp>
#include <vinc_messages/int3.hpp>
#include <iostream>

using vinc::int3;

class RpcExample final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    RpcExample() : BaseModule("rpcCpp") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        // Capacity includes the complete encoded message, not only its data fields.
        vinc::rpc::ServiceOptions options;
        return context.register_rpc<int3, int3>("echo", options, [](const int3& request, int3& response) {
            response.data().x = request.data().x.get();
            response.data().y = request.data().y.get();
            response.data().z = request.data().z.get();
            return vinc::rpc::RpcStatus::success();
        });
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        // Shell keeps the addressed Module name as argv[0].
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        if (args.size() != 1) {
            std::cout << "usage: rpcCpp <target-module>\n";
            return 1;
        }

        auto request = vinc::make_message<int3>();
        request->data().x = 42;
        auto submitted = context.call_rpc<int3, int3>(
            args.front(), "echo", *request, {}, [](vinc::Result<vinc::rpc::RpcReply<int3>> reply) {
                // This completion runs on a later pump, without blocking the Shell hook.
                if (!reply) {
                    std::cerr << vinc::error_to_string(reply.error()) << '\n';
                    return;
                }

                std::cout << "RPC status=" << reply->status.code << " reason=" << reply->status.reason
                          << " x=" << reply->response->data().x.get() << '\n';
                // reply->response is independently owned and may be moved elsewhere.
            });
        if (!submitted) {
            std::cerr << vinc::error_to_string(submitted.error()) << '\n';
            return 1;
        }

        // Admission here means locally queued. The completion reports remote QueueFull.
        return 0;
    }
};

REGISTER_MODULE(RpcExample, "rpcExample")
