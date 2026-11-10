#pragma once
#include <vinc/rpc/detail/access.hpp>
#include <vinc/rpc/detail/call_endpoint.hpp>
#include <vinc/rpc/messages.hpp>
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

using Handler = std::function<Result<RawReply>(control::Bytes, std::uint64_t)>;
}

// Context owns this facade and all application closures. The Host owns Access.
class Client {
    friend struct detail::ClientAccess;

public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Result<PumpStats> pump();
    Stats stats() const;

private:
    class Endpoint;
    std::shared_ptr<Endpoint> endpoint_;
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
        std::shared_ptr<detail::CallReservation> reservation;
    };

    std::map<std::uint64_t, std::shared_ptr<Pending>> calls_;
    std::shared_ptr<detail::CallBudget> budget_ = std::make_shared<detail::CallBudget>();
    Stats stats_;
    Stats synchronous_stats_;
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
    Result<detail::RawResult> call_sync_raw(const detail::Address& address, std::uint64_t request_hash,
        std::uint64_t response_hash, message::Snapshot image, CallOptions options, Deadline deadline);

public:
    template <Message Request, Message Response>
    Result<RpcStatus> call_sync(const detail::Address& address, RpcMessages<Request, Response>& messages,
                               CallOptions options) {
        if (options.timeout.count() <= 0 || options.timeout > config::rpc_max_call_timeout) {
            return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Call});
        }

        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        auto image = messages.request().snapshot(config::rpc_local_bytes);
        if (!image) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Call, image.error()));
        }

        auto result = call_sync_raw(address, Request::schema().hash, Response::schema().hash,
                                    std::move(*image), options, deadline);
        if (!result) {
            return std::unexpected(result.error());
        }

        auto binding = message::detail::adopt_record(std::move(result->image), Response::schema());
        if (!binding) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Receive, binding.error()));
        }

        auto response = message::detail::RecordAccess::bind<Response>(std::move(*binding));
        auto assigned = messages.response().assign(response);
        if (!assigned) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Receive, assigned.error()));
        }

        return std::move(result->status);
    }

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


};

namespace detail {
struct ClientAccess {
    static std::weak_ptr<CallEndpoint> endpoint(Client& client);
    static Result<RawResult> call_sync(Client& client, const Address& address,
        std::uint64_t request_hash, std::uint64_t response_hash, message::Snapshot image,
        CallOptions options, Deadline deadline) {
        return client.call_sync_raw(address, request_hash, response_hash, std::move(image), options, deadline);
    }

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
