#pragma once
#include <vinc/rpc/detail/access.hpp>
#include <vinc/message/generated.hpp>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace vinc::rpc {
namespace detail {
struct ClientAccess;
}

namespace detail {
struct RawReply {
    RpcStatus status;
    message::Snapshot image;
};

struct RawResult {
    RpcStatus status;
    control::Bytes image;
};

using Handler = std::function<Result<RawReply>(control::Bytes, std::uint64_t)>;
using Completion = std::function<void(Result<RawResult>)>;
}

// Context owns this facade and all application closures. The Host owns Access.
class Client {
    friend struct detail::ClientAccess;

public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Result<PumpStats> pump();
    Stats stats() const;

private:
    detail::Access* access_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex admission_;
    std::map<std::uint64_t, detail::Handler> handlers_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> invoked_;

    struct Pending {
        message::Snapshot image;
        detail::Completion completion;
        Deadline deadline;
        std::optional<detail::Transfer> transfer;
        bool notified = false;
    };

    std::map<std::uint64_t, std::shared_ptr<Pending>> calls_;
    std::uint64_t local_bytes_ = 0;
    Stats stats_;
    Result<void> register_raw(std::string name, std::uint64_t request_hash, std::uint64_t response_hash,
                              ServiceOptions options, detail::Handler handler);
    Result<void> call_raw(std::string target, std::string service, std::uint64_t request_hash,
                          std::uint64_t response_hash, message::Snapshot image, CallOptions options,
                          detail::Completion completion);
    Result<void> grant(const control::Frame& frame);
    Result<void> invoke(const control::Frame& frame);
    Result<void> complete(const control::Frame& frame);
    Result<void> release(std::uint64_t token);
    void finish(std::uint64_t id, const std::shared_ptr<Pending>& pending, Result<detail::RawResult> result);

public:
    template <class Request, class Response, class Handler>
    Result<void> register_rpc(std::string name, ServiceOptions options, Handler handler) {
        static_assert(std::is_base_of_v<message::GeneratedMessage<Request>, Request>);
        static_assert(std::is_base_of_v<message::GeneratedMessage<Response>, Response>);
        return register_raw(
            std::move(name), Request::schema().hash, Response::schema().hash, options,
            [handler = std::move(handler)](control::Bytes bytes,
                                           std::uint64_t maximum) mutable -> Result<detail::RawReply> {
                auto binding = message::detail::adopt_record(std::move(bytes), Request::schema());
                if (!binding) {
                    return std::unexpected(
                        wrap_error(ErrorCode::StorageFailed, ErrorOperation::Receive, binding.error()));
                }

                auto request = message::detail::RecordAccess::bind<Request>(std::move(*binding));
                auto response = vinc::make_message<Response>();
                auto status = handler(std::as_const(request), *response);
                auto image = response->snapshot(maximum);
                if (!image) {
                    return std::unexpected(
                        wrap_error(ErrorCode::StorageFailed, ErrorOperation::Send, image.error()));
                }

                return detail::RawReply{std::move(status), std::move(*image)};
            });
    }

    template <class Request, class Response, class Completion>
    Result<void> call_rpc(std::string target, std::string service, const Request& request,
                          CallOptions options, Completion completion) {
        static_assert(std::is_base_of_v<message::GeneratedMessage<Request>, Request>);
        static_assert(std::is_base_of_v<message::GeneratedMessage<Response>, Response>);
        auto image = request.snapshot(config::rpc_local_bytes);
        if (!image) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Call, image.error()));
        }

        return call_raw(std::move(target), std::move(service), Request::schema().hash,
                        Response::schema().hash, std::move(*image), options,
                        [completion = std::move(completion)](Result<detail::RawResult> result) mutable {
                            if (!result) {
                                completion(Result<RpcReply<Response>>{std::unexpected(result.error())});
                                return;
                            }

                            auto binding =
                                message::detail::adopt_record(std::move(result->image), Response::schema());
                            if (!binding) {
                                completion(Result<RpcReply<Response>>{std::unexpected(wrap_error(
                                    ErrorCode::StorageFailed, ErrorOperation::Receive, binding.error()))});
                                return;
                            }

                            auto response = std::make_unique<Response>(
                                message::detail::RecordAccess::bind<Response>(std::move(*binding)));
                            completion(Result<RpcReply<Response>>{
                                RpcReply<Response>{std::move(result->status), std::move(response)}});
                        });
    }
};

namespace detail {
struct ClientAccess {
    static void bind(Client& client, Access& access) {
        client.access_ = &access;
    }

    static Result<void> register_raw(Client& client, std::string name, std::uint64_t request_hash,
                                     std::uint64_t response_hash, ServiceOptions options, Handler handler) {
        return client.register_raw(std::move(name), request_hash, response_hash, options, std::move(handler));
    }

    static Result<void> call_raw(Client& client, std::string target, std::string name,
                                 std::uint64_t request_hash, std::uint64_t response_hash,
                                 message::Snapshot image, CallOptions options, Completion completion) {
        return client.call_raw(std::move(target), std::move(name), request_hash, response_hash,
                               std::move(image), options, std::move(completion));
    }
};
}
}
