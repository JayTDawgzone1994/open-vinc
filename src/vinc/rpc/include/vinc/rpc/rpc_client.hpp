#pragma once
#include <vinc/rpc/messages.hpp>
#include <vinc/rpc/detail/async_call.hpp>

namespace vinc::rpc {
// Typed facade over the shared C++/language-adapter admission and callback state.
template <Message Request, Message Response>
class RpcClient {
    friend struct detail::RpcClientAccess;
    detail::AsyncCall call_;

    explicit RpcClient(detail::AsyncCall call) : call_(std::move(call)) {
    }

    template <class Completion>
    static Result<RpcClient> create(std::weak_ptr<detail::CallEndpoint> endpoint,
        std::string_view address, Completion completion, CallOptions options) {
        std::function<void(Result<RpcReply<Response>>)> callback(std::move(completion));
        if (!callback) {
            return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Create});
        }

        auto call = detail::AsyncCall::create(std::move(endpoint), address,
            Request::schema().hash, Response::schema().hash,
            [completion = std::move(callback)](Result<detail::RawResult> result) mutable {
                completion(decode_reply(std::move(result)));
            }, options);
        if (!call) {
            return std::unexpected(call.error());
        }

        return RpcClient(std::move(*call));
    }

    static Result<RpcReply<Response>> decode_reply(Result<detail::RawResult> result) {
        if (!result) {
            return std::unexpected(result.error());
        }

        auto binding = message::detail::adopt_record(std::move(result->image), Response::schema());
        if (!binding) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Receive, binding.error()));
        }

        auto response = std::make_unique<Response>(
            message::detail::RecordAccess::bind<Response>(std::move(*binding)));
        return RpcReply<Response>{std::move(result->status), std::move(response)};
    }

public:
    RpcClient() = default;
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) = default;
    RpcClient& operator=(RpcClient&&) = default;

    std::unique_ptr<Request> make_request() const {
        return vinc::make_message<Request>();
    }

    void close() {
        call_.close();
    }

    Result<void> call(const Request& request) {
        return call_.call([&] {
            return request.snapshot(config::rpc_local_bytes);
        });
    }

    Result<void> call(const Request& request, CallOptions options) {
        return call_.call([&] {
            return request.snapshot(config::rpc_local_bytes);
        }, options);
    }
};
}
