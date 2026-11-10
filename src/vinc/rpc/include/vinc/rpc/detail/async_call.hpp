#pragma once
#include <vinc/rpc/detail/call_endpoint.hpp>
#include <mutex>

namespace vinc::rpc::detail {
// A Context-issued application handle. The endpoint is weak: retaining this
// handle cannot retain the Context or create a second control-service owner.
class AsyncCall {
    using Reply = Result<RawResult>;
    using Callback = std::function<void(Reply)>;

    struct Pending {};

    struct State {
        detail::Address address;
        std::uint64_t request_hash = 0;
        std::uint64_t response_hash = 0;
        CallOptions options;
        std::weak_ptr<detail::CallEndpoint> endpoint;
        std::mutex mutex;
        std::shared_ptr<Callback> callback;
        std::shared_ptr<Pending> active;
    };

    // Roll back only this admission, including exceptions from ordinary local
    // allocation. An older completion must never clear a newer request's state.
    struct Admission {
        std::shared_ptr<State> state;
        std::shared_ptr<Pending> pending;
        bool accepted = false;

        ~Admission() {
            if (!accepted) {
                std::lock_guard lock(state->mutex);
                if (state->active == pending) {
                    state->active.reset();
                }
            }
        }
    };

    std::shared_ptr<State> state_;

    explicit AsyncCall(std::shared_ptr<State> state) : state_(std::move(state)) {
    }

    static bool valid_options(CallOptions options) {
        return options.timeout.count() > 0 && options.timeout <= config::rpc_max_call_timeout;
    }

    static void complete(const std::shared_ptr<State>& state, const std::shared_ptr<Pending>& pending,
                         Result<detail::RawResult> result) {
        std::shared_ptr<Callback> callback;
        {
            std::lock_guard lock(state->mutex);
            if (!state->callback || state->active != pending) {
                return;
            }

            // Dispatch starts here. Release busy before application code can
            // submit again; close does not join a dispatch already started.
            state->active.reset();
            callback = state->callback;
        }

        (*callback)(std::move(result));
    }

public:
    template <class Completion>
    static Result<AsyncCall> create(std::weak_ptr<detail::CallEndpoint> endpoint,
                                    std::string_view address, std::uint64_t request_hash, std::uint64_t response_hash,
                                    Completion completion, CallOptions options) {
        auto parsed = detail::parse_address(address);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        if (!valid_options(options)) {
            return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Create});
        }

        if (endpoint.expired()) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Create});
        }

        auto callback = std::make_shared<Callback>(std::move(completion));
        if (!*callback) {
            return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Create});
        }

        auto state = std::make_shared<State>();
        state->address = std::move(*parsed);
        state->options = options;
        state->request_hash = request_hash;
        state->response_hash = response_hash;
        state->endpoint = std::move(endpoint);
        state->callback = std::move(callback);
        return AsyncCall(std::move(state));
    }

    // Empty handles are useful as Module members before on_start. They have no
    // authority: call returns Closed until a Context-issued handle is moved in.
    AsyncCall() = default;
    AsyncCall(const AsyncCall&) = delete;
    AsyncCall& operator=(const AsyncCall&) = delete;
    AsyncCall(AsyncCall&&) = default;

    AsyncCall& operator=(AsyncCall&& other) {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
        }

        return *this;
    }

    ~AsyncCall() {
        close();
    }

    void close() {
        auto state = std::move(state_);
        if (!state) {
            return;
        }

        std::shared_ptr<Callback> retired;
        {
            std::lock_guard lock(state->mutex);
            retired = std::move(state->callback);
            state->active.reset();
        }
        // Destroy application captures outside the state mutex. The endpoint
        // still owns transport cleanup; closing a handle is not remote cancel.
    }

    template <class SnapshotFactory>
    Result<void> call(SnapshotFactory make_image) {
        if (!state_) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
        }

        return call(std::move(make_image), state_->options);
    }

    template <class SnapshotFactory>
    Result<void> call(SnapshotFactory make_image, CallOptions options) {
        const auto state = state_;
        if (!state) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
        }

        if (!valid_options(options)) {
            return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Call});
        }

        auto endpoint = state->endpoint.lock();
        if (!endpoint) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
        }

        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(state->mutex);
            if (!state->callback) {
                return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
            }

            if (state->active) {
                return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Call});
            }

            pending = std::make_shared<Pending>();
            state->active = pending;
        }

        Admission admission{state, pending};
        auto image = make_image();
        if (!image) {
            return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Call, image.error()));
        }

        auto submitted = endpoint->submit(
            state->address, state->request_hash, state->response_hash, std::move(*image), options,
            [weak = std::weak_ptr<State>(state), pending](Result<detail::RawResult> result) {
                if (auto current = weak.lock()) {
                    complete(current, pending, std::move(result));
                }
            });
        if (!submitted) {
            return std::unexpected(submitted.error());
        }

        admission.accepted = true;
        return {};
    }
};
}
