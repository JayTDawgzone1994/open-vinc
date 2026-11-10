#include <vinc/rpc/detail/host.hpp>
#include <vinc/rpc/detail/storage.hpp>
#include <bit>

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
    return send_locked(std::move(request));
}

Result<void> Host::send_locked(control::Frame request) {
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
    if (!living()) {
        // Normal Module completion closes admission before the watchdog exits.
        // Do not turn deferred Release into a new fault during that shutdown.
        // Central recovery retains ownership until actual process exit.
        return {};
    }

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

    return retire_synchronous();
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

    if (frame.operation == Grant || frame.operation == Reply) {
        if (auto found = synchronous_.find(frame.request); found != synchronous_.end()) {
            return receive_synchronous(*found->second, std::move(frame));
        }
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

class Host::Waiter final : public BlockingCall {
    Host& host_;
    std::shared_ptr<Synchronous> call_;

public:
    Waiter(Host& host, std::shared_ptr<Synchronous> call) : host_(host), call_(std::move(call)) {
    }

    ~Waiter() override {
        host_.abandon(call_);
    }

    Result<RawResult> wait(std::span<const std::byte> image, Deadline deadline) override {
        return host_.wait_call(*call_, image, deadline);
    }
};

Result<std::unique_ptr<BlockingCall>> Host::start_call(
    control::Frame request, std::shared_ptr<CallReservation> reservation) {
    auto call = std::make_shared<Synchronous>();
    call->id = request.request;
    call->reservation = std::move(reservation);
    // Declare the ticket before the lock: failure unwinds the lock before the
    // ticket transfers cleanup. The Host outlives all entered Context calls.
    std::unique_ptr<BlockingCall> waiter = std::make_unique<Waiter>(*this, call);
    std::lock_guard lock(mutex_);
    if (!living()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
    }

    if (synchronous_.size() >= config::rpc_pending_limit) {
        return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Call});
    }

    if (!call->reservation || request.kind != control::Kind::Request || request.domain != domain ||
        request.operation != Acquire || !call->id || synchronous_.contains(call->id)) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Call});
    }

    synchronous_.emplace(call->id, call);
    try {
        auto sent = send_locked(std::move(request));
        if (!sent) {
            synchronous_.erase(call->id);
            return std::unexpected(sent.error());
        }
    } catch (...) {
        // send_locked either enqueues fully or throws before accepting. Preserve
        // the existing allocation policy, without retaining an unsent call.
        synchronous_.erase(call->id);
        throw;
    }

    return waiter;
}

Result<void> Host::receive_synchronous(Synchronous& call, control::Frame frame) {
    if ((frame.operation == Grant && call.grant_seen) || (frame.operation == Reply && call.reply_seen)) {
        return {};
    }

    auto response = decode_response(frame.payload);
    if (!response) {
        return std::unexpected(response.error());
    }

    if (frame.operation == Grant) {
        if (!response->failure) {
            auto transfer = decode_transfer(response->body);
            if (!transfer) {
                return std::unexpected(transfer.error());
            }

            call.transfer = std::move(*transfer);
        }

        call.grant = std::move(frame);
        call.grant_seen = true;
    } else {
        if (!call.transfer || !call.submitted) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }

        if (!response->failure) {
            control::Reader reader(response->body);
            (void)reader.u32();
            auto reason = reader.string();
            const auto size = reader.u64();
            if (!reader.done() || reason.size() > config::rpc_reason_bytes ||
                size > call.transfer->response_bytes) {
                return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
            }
        }

        call.reply = std::move(frame);
        call.reply_seen = true;
    }

    changed_.notify_all();
    return {};
}

Result<control::Frame> Host::wait_event(Synchronous& call, bool reply, Deadline deadline) {
    std::unique_lock lock(mutex_);
    auto& event = reply ? call.reply : call.grant;
    changed_.wait_until(lock, deadline, [&] {
        return event.has_value() || !living();
    });
    if (!living()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(Error{ErrorCode::TimeoutOutcomeUnknown, ErrorOperation::Call});
    }

    auto frame = std::move(*event);
    event.reset();
    return frame;
}

Result<RawResult> Host::wait_call(Synchronous& call, std::span<const std::byte> image, Deadline deadline) {
    auto grant = wait_event(call, false, deadline);
    if (!grant) {
        return std::unexpected(grant.error());
    }

    auto granted = decode_response(grant->payload);
    if (!granted) {
        return std::unexpected(granted.error());
    }

    if (granted->failure) {
        return std::unexpected(*granted->failure);
    }

    const auto transfer = *call.transfer;
    {
        auto memory = TransferMemory::open(transfer);
        if (!memory) {
            return std::unexpected(memory.error());
        }

        auto written = memory->write(false, image);
        if (!written) {
            return std::unexpected(written.error());
        }
    }

    control::Bytes bytes;
    control::put_u64(bytes, image.size());
    {
        std::lock_guard lock(mutex_);
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::TimeoutOutcomeUnknown, ErrorOperation::Call});
        }

        auto sent = send_locked(frame(Submit, transfer.token, std::move(bytes)));
        if (!sent) {
            return std::unexpected(sent.error());
        }

        // Host reception takes this same mutex. A Reply cannot race this
        // ownership handoff; an unaccepted Submit remains releasable Granted.
        call.submitted = true;
    }

    auto reply = wait_event(call, true, deadline);
    if (!reply) {
        return std::unexpected(reply.error());
    }

    auto response = decode_response(reply->payload);
    if (!response) {
        return std::unexpected(response.error());
    }

    if (response->failure) {
        return std::unexpected(*response->failure);
    }

    control::Reader reader(response->body);
    RpcStatus status;
    status.code = std::bit_cast<std::int32_t>(reader.u32());
    status.reason = reader.string();
    const auto size = reader.u64();
    if (!reader.done()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    auto memory = TransferMemory::open(transfer);
    if (!memory) {
        return std::unexpected(memory.error());
    }

    auto copied = memory->read(true, size);
    if (!copied) {
        return std::unexpected(copied.error());
    }

    return RawResult{std::move(status), std::move(*copied)};
}

void Host::abandon(const std::shared_ptr<Synchronous>& call) noexcept {
    // Only mark state here: destructors must not allocate a Release frame or
    // run user code. progress() retires it without requiring an application pump.
    std::lock_guard lock(mutex_);
    call->abandoned = true;
    changed_.notify_all();
}

Result<void> Host::retire_synchronous() {
    for (auto current = synchronous_.begin(); current != synchronous_.end();) {
        const auto& call = *current->second;
        const bool waiting_for_peer = !call.grant_seen || (call.submitted && !call.reply_seen);
        if (!call.abandoned || waiting_for_peer) {
            ++current;
            continue;
        }

        if (call.transfer) {
            auto released = send_locked(frame(Release, call.transfer->token));
            if (!released) {
                if (released.error().is(ErrorCode::QueueFull)) {
                    break; // Keep the token/budget and retry after output drains.
                }

                return released;
            }
        }

        current = synchronous_.erase(current);
    }

    return {};
}
}
