#include <vinc/rpc/detail/host.hpp>

namespace vinc::rpc::detail {
Host::Host(std::shared_ptr<control::Session> session) : session_(std::move(session)) {
}

void Host::stop() noexcept {
    stopped_.store(true);
    changed_.notify_all();
}

bool Host::living() const noexcept {
    return !stopped_.load();
}

bool Host::ready() const noexcept {
    return ready_.load();
}

Result<std::uint64_t> Host::reserve_request() {
    std::lock_guard lock(mutex_);
    if (next_request_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::IdExhausted, ErrorOperation::Call});
    }

    return next_request_++;
}

Result<void> Host::send(control::Frame request) {
    std::lock_guard lock(mutex_);
    if (!living()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Send});
    }

    if (output_.size() >= config::rpc_control_limit) {
        return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Send});
    }

    if (output_.empty()) {
        auto sent = session_->send(request);
        if (sent) {
            return {};
        }

        if (!sent.error().is(control::ErrorCode::Backpressure)) {
            return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Send, sent.error()));
        }
    }

    output_.push_back(std::move(request));
    return {};
}

Result<void> Host::progress() {
    std::lock_guard lock(mutex_);
    while (!output_.empty()) {
        auto sent = session_->send(output_.front());
        if (!sent) {
            if (sent.error().is(control::ErrorCode::Backpressure)) {
                return {};
            }

            return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Send, sent.error()));
        }

        output_.pop_front();
    }

    return {};
}

Result<control::Bytes> Host::exchange(control::Frame request) {
    auto pending = std::make_shared<Pending>();
    const auto id = request.request;
    {
        std::lock_guard lock(mutex_);
        if (pending_.size() >= config::rpc_pending_limit) {
            return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Register});
        }

        pending_.emplace(id, pending);
    }

    auto sent = send(std::move(request));
    std::unique_lock lock(mutex_);
    if (!sent) {
        pending_.erase(id);
        return std::unexpected(sent.error());
    }

    changed_.wait_for(lock, config::rpc_call_timeout, [&] {
        return pending->done || !living();
    });
    pending_.erase(id);
    if (!pending->done) {
        const auto code = living() ? ErrorCode::TimeoutOutcomeUnknown : ErrorCode::Closed;
        return std::unexpected(Error{code, ErrorOperation::Register});
    }

    auto response = decode_response(pending->bytes);
    if (!response) {
        return std::unexpected(response.error());
    }

    if (response->failure) {
        return std::unexpected(*response->failure);
    }

    return control::Bytes(response->body.begin(), response->body.end());
}

Result<void> Host::receive(control::Frame frame) {
    if (frame.kind != control::Kind::Response || frame.domain != domain || !frame.request) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    std::lock_guard lock(mutex_);
    if (frame.operation == Register) {
        if (auto found = pending_.find(frame.request); found != pending_.end()) {
            if (found->second->done) {
                return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
            }

            found->second->bytes = std::move(frame.payload);
            found->second->done = true;
            changed_.notify_all();
        }

        return {}; // An expired registration waiter cannot revive its callback.
    }

    if ((frame.operation != Grant && frame.operation != Invoke && frame.operation != Reply) ||
        input_.size() >= config::rpc_control_limit) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    input_.push_back(std::move(frame));
    ready_.store(true);
    return {};
}

std::deque<control::Frame> Host::take() {
    std::lock_guard lock(mutex_);
    std::deque<control::Frame> result;
    result.swap(input_);
    ready_.store(false);
    return result;
}
}
