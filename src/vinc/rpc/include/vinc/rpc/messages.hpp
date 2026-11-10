#pragma once
#include <vinc/message/generated.hpp>
#include <concepts>

namespace vinc::rpc {
template <class T>
concept Message = std::derived_from<T, message::GeneratedMessage<T>>;

// Both messages own ordinary process-local storage. This is not a shared slot
// or a new wire schema. Whole-message replacement retains Storage's Result API.
template <Message Request, Message Response>
class RpcMessages {
public:
    RpcMessages() = default;
    RpcMessages(const RpcMessages&) = default;
    RpcMessages(RpcMessages&&) = default;
    RpcMessages& operator=(const RpcMessages&) = delete;
    RpcMessages& operator=(RpcMessages&&) = delete;

    Request& request() noexcept {
        return request_;
    }

    const Request& request() const noexcept {
        return request_;
    }

    Response& response() noexcept {
        return response_;
    }

    const Response& response() const noexcept {
        return response_;
    }

private:
    Request request_;
    Response response_;
};
}

namespace vinc {
template <rpc::Message Request, rpc::Message Response>
rpc::RpcMessages<Request, Response> make_rpc_messages() {
    return {};
}
}
