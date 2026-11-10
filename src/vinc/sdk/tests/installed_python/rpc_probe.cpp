#include <vinc/vinc.hpp>
#include <vinc_messages/SdkPacket.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using Packet = vinc::SdkPacket;
constexpr auto test_timeout = std::chrono::seconds(8);
constexpr std::size_t reply_count = 2;

template <class T>
auto take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

class RpcProbe final : public vinc::BaseModule<vinc::RunMode::Manual> {
    bool ready_ = false;

public:
    RpcProbe() : BaseModule("rpcProbe") {
    }

    vinc::Result<void> main() override {
        auto messages = vinc::make_rpc_messages<Packet, Packet>();
        messages.request().data().value = Packet::revision;
        // No caller pump or helper thread has run. The installed Host owns transport.
        auto status = take(context.call_rpc("py.echo", messages));
        check(status.code == 0 && messages.response().data().value.get() == Packet::revision + 3,
              "installed C++ to Python synchronous response");
        auto missing = context.call_rpc("py.missing", messages);
        check(!missing && missing.error().is(vinc::rpc::ErrorCode::NotFound), "missing service classification");
        check(messages.response().data().value.get() == Packet::revision + 3, "failed call preserves response");

        vinc::rpc::RpcClient<Packet, Packet> client;
        std::vector<std::unique_ptr<Packet>> replies;
        auto request = vinc::make_message<Packet>();
        request->data().value = Packet::revision;
        client = take(context.rpc_client<Packet, Packet>("py.echo", [&](auto result) {
            auto reply = take(std::move(result));
            check(reply.status.code == 0, "installed asynchronous status");
            replies.push_back(std::move(reply.response));
            auto blocked = context.call_rpc("py.echo", messages);
            check(!blocked && blocked.error().is(vinc::rpc::ErrorCode::BlockingCallInPump),
                  "installed Plugin pump guard");
            auto waiting = context.waitForWork();
            check(!waiting && waiting.error().is(vinc::module_system::ErrorCode::Busy) &&
                  waiting.error().is_operation(vinc::module_system::ErrorOperation::WaitForWork),
                  "installed Plugin wait guard");
            if (replies.size() < reply_count) {
                request->data().value = Packet::revision + 1;
                take(client.call(*request));
            }
        }));
        take(client.call(*request));
        auto busy = client.call(*request);
        check(!busy && busy.error().is(vinc::rpc::ErrorCode::Busy), "installed reusable client Busy");
        const auto deadline = std::chrono::steady_clock::now() + test_timeout;
        while (replies.size() < reply_count) {
            check(context.living() && std::chrono::steady_clock::now() < deadline, "installed RPC deadline");
            const auto batch = take(context.pumpOnce());
            if (batch.callbacks == 0 && batch.commands == 0) {
                take(context.waitForWork(deadline));
            }
        }

        client.close();
        check(replies.front()->data().value.get() == Packet::revision + 3 &&
              replies.back()->data().value.get() == Packet::revision + 4, "independent retained replies");
        ready_ = true;
        std::cout << "INSTALLED_CPP_RPC_OK\n" << std::flush;
        std::cout << "INSTALLED_CPP_WAIT_OK\n" << std::flush;
        return context.pumpLoop();
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return ready_ ? 0 : 1;
    }
};
}

REGISTER_MODULE(RpcProbe, "RpcProbe")
