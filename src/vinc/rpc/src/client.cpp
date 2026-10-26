#include <vinc/rpc/client.hpp>
#include <vinc/rpc/detail/storage.hpp>
#include <bit>

namespace vinc::rpc {
Result<void> Client::register_raw(std::string name, std::uint64_t request_hash, std::uint64_t response_hash,
                                  ServiceOptions options, detail::Handler handler) {
    if (!detail::valid_name(name) || !handler) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }

    auto extent = detail::pool_bytes(options);
    if (!extent) {
        return std::unexpected(extent.error());
    }

    // Serialize reserve+enqueue so request numbers follow wire order across callers.
    std::lock_guard admission(admission_);
    if (!access_ || !access_->living()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Register});
    }

    auto id = access_->reserve_request();
    if (!id) {
        return std::unexpected(id.error());
    }

    control::Bytes bytes;
    control::put_u32(bytes, detail::version);
    (void)control::put_string(bytes, name);
    control::put_u64(bytes, request_hash);
    control::put_u64(bytes, response_hash);
    control::put_u64(bytes, options.request_bytes);
    control::put_u64(bytes, options.response_bytes);
    control::put_u32(bytes, options.slots);
    // Publish the handler before a concurrent pump can dispatch its first Invoke.
    // The Host receiver never takes this mutex, so registration can still finish.
    std::lock_guard lock(mutex_);
    auto result = access_->exchange(detail::frame(detail::Register, *id, std::move(bytes)));
    if (!result) {
        return std::unexpected(result.error());
    }

    control::Reader reader(*result);
    const auto service = reader.u64();
    if (!reader.done() || !service) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Register});
    }

    handlers_.emplace(service, std::move(handler));
    return {};
}

Result<void> Client::call_raw(std::string target, std::string service, std::uint64_t request_hash,
                              std::uint64_t response_hash, message::Snapshot image, CallOptions options,
                              detail::Completion completion) {
    if (!detail::valid_name(target) || !detail::valid_name(service) || !completion ||
        options.timeout.count() <= 0 || options.timeout > config::rpc_max_call_timeout) {
        return std::unexpected(Error{ErrorCode::InvalidOptions, ErrorOperation::Call});
    }

    std::lock_guard admission(admission_);
    if (!access_ || !access_->living()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call});
    }

    auto pending = std::make_shared<Pending>();
    pending->image = std::move(image);
    pending->completion = std::move(completion);
    pending->deadline = std::chrono::steady_clock::now() + options.timeout;
    auto id = access_->reserve_request();
    if (!id) {
        return std::unexpected(id.error());
    }

    control::Bytes bytes;
    control::put_u32(bytes, detail::version);
    (void)control::put_string(bytes, target);
    (void)control::put_string(bytes, service);
    control::put_u64(bytes, request_hash);
    control::put_u64(bytes, response_hash);
    control::put_u64(bytes, pending->image.bytes().size());
    std::lock_guard lock(mutex_);
    const auto size = pending->image.bytes().size();
    if (calls_.size() >= config::rpc_pending_limit || size > config::rpc_local_bytes - local_bytes_) {
        ++stats_.queue_full;
        return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Call});
    }

    calls_.emplace(*id, pending);
    auto sent = access_->send(detail::frame(detail::Acquire, *id, std::move(bytes)));
    if (!sent) {
        calls_.erase(*id);
        if (sent.error().is(ErrorCode::QueueFull)) {
            ++stats_.queue_full;
        }

        return std::unexpected(sent.error());
    }

    local_bytes_ += size;
    return {};
}

Stats Client::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.pending = calls_.size();
    result.request_bytes = local_bytes_;
    return result;
}

Result<void> Client::release(std::uint64_t token) {
    return access_->send(detail::frame(detail::Release, token));
}

void Client::finish(std::uint64_t id, const std::shared_ptr<Pending>& pending,
                    Result<detail::RawResult> result) {
    {
        std::lock_guard lock(mutex_);
        local_bytes_ -= pending->image.bytes().size();
        calls_.erase(id);
        if (!pending->notified) {
            ++stats_.completions;
            if (!result && result.error().is(ErrorCode::QueueFull)) {
                ++stats_.queue_full;
            }
        }
    }

    if (!pending->notified) {
        pending->notified = true;
        pending->completion(std::move(result));
    }
}

Result<void> Client::grant(const control::Frame& frame) {
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lock(mutex_);
        auto found = calls_.find(frame.request);
        if (found == calls_.end()) {
            return {}; // Retired request IDs never revive callbacks.
        }

        pending = found->second;
    }

    auto response = detail::decode_response(frame.payload);
    if (!response) {
        return std::unexpected(response.error());
    }

    if (response->failure) {
        finish(frame.request, pending, std::unexpected(*response->failure));
        return {};
    }

    auto transfer = detail::decode_transfer(response->body);
    if (!transfer) {
        return std::unexpected(transfer.error());
    }

    if (pending->transfer) {
        return {}; // Duplicate grant must not overwrite a submitted image.
    }

    pending->transfer = *transfer;
    if (pending->notified) {
        auto released = release(transfer->token);
        if (!released) {
            return released;
        }

        finish(frame.request, pending,
               std::unexpected(Error{ErrorCode::TimeoutOutcomeUnknown, ErrorOperation::Call}));
        return {};
    }

    auto memory = detail::TransferMemory::open(*transfer);
    Result<void> written;
    if (!memory) {
        written = std::unexpected(memory.error());
    } else {
        written = memory->write(false, pending->image.bytes());
    }

    if (!written) {
        auto released = release(transfer->token);
        if (!released) {
            return released;
        }

        finish(frame.request, pending, std::unexpected(written.error()));
        return {};
    }

    control::Bytes bytes;
    control::put_u64(bytes, pending->image.bytes().size());
    return access_->send(detail::frame(detail::Submit, transfer->token, std::move(bytes)));
}

Result<void> Client::invoke(const control::Frame& frame) {
    auto response = detail::decode_response(frame.payload);
    if (!response || response->failure) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    auto transfer = detail::decode_transfer(response->body);
    if (!transfer || transfer->token != frame.request) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    // Each registered slot has a monotonically increasing token. Remembering its
    // last invocation is bounded by service capacity, unlike an unbounded ID set.
    auto& previous = invoked_[{transfer->service, transfer->offset}];
    if (transfer->token <= previous) {
        return {};
    }

    previous = transfer->token;
    detail::Handler handler;
    {
        std::lock_guard lock(mutex_);
        auto found = handlers_.find(transfer->service);
        if (found != handlers_.end()) {
            handler = found->second;
        }
    }

    auto result = [&]() -> Result<control::Bytes> {
        if (!handler) {
            return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Call});
        }

        auto memory = detail::TransferMemory::open(*transfer);
        if (!memory) {
            return std::unexpected(memory.error());
        }

        auto image = memory->read(false, transfer->size);
        if (!image) {
            return std::unexpected(image.error());
        }

        auto result = handler(std::move(*image), transfer->response_bytes);
        {
            std::lock_guard lock(mutex_);
            ++stats_.requests;
        }

        if (!result) {
            return std::unexpected(result.error());
        }

        if (result->status.reason.size() > config::rpc_reason_bytes) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Send});
        }

        auto written = memory->write(true, result->image.bytes());
        if (!written) {
            return std::unexpected(written.error());
        }

        control::Bytes bytes;
        control::put_u32(bytes, detail::result_success);
        control::put_u32(bytes, std::bit_cast<std::uint32_t>(result->status.code));
        (void)control::put_string(bytes, result->status.reason);
        control::put_u64(bytes, result->image.bytes().size());
        return bytes;
    }();
    auto bytes = result ? std::move(*result) : detail::encode_failure(result.error());
    return access_->send(detail::frame(detail::Complete, frame.request, std::move(bytes)));
}

Result<void> Client::complete(const control::Frame& frame) {
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lock(mutex_);
        auto found = calls_.find(frame.request);
        if (found == calls_.end()) {
            return {};
        }

        pending = found->second;
    }

    if (!pending->transfer) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    auto result = [&]() -> Result<detail::RawResult> {
        auto response = detail::decode_response(frame.payload);
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
        if (!reader.done() || status.reason.size() > config::rpc_reason_bytes) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }

        if (pending->notified) {
            return detail::RawResult{};
        }

        auto memory = detail::TransferMemory::open(*pending->transfer);
        if (!memory) {
            return std::unexpected(memory.error());
        }

        auto image = memory->read(true, size);
        if (!image) {
            return std::unexpected(image.error());
        }

        return detail::RawResult{std::move(status), std::move(*image)};
    }();
    auto released = release(pending->transfer->token);
    if (!released) {
        return released;
    }

    finish(frame.request, pending, std::move(result));
    return {};
}

Result<PumpStats> Client::pump() {
    PumpStats stats;
    if (!access_ || !access_->living()) {
        return stats;
    }

    auto batch = access_->take();
    const auto before = this->stats();
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Pending>>> expired;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex_);
        for (auto& entry : calls_) {
            if (!entry.second->notified && now >= entry.second->deadline) {
                expired.push_back(entry);
            }
        }
    }

    try {
        for (auto& [id, pending] : expired) {
            pending->notified = true;
            auto completion = std::move(pending->completion);
            {
                std::lock_guard lock(mutex_);
                ++stats_.timeouts;
                ++stats_.completions;
            }

            completion(std::unexpected(Error{ErrorCode::TimeoutOutcomeUnknown, ErrorOperation::Call}));
        }

        for (auto& frame : batch) {
            if (!access_->living()) {
                break;
            }

            Result<void> result;
            if (frame.operation == detail::Grant) {
                result = grant(frame);
            } else if (frame.operation == detail::Invoke) {
                result = invoke(frame);
            } else if (frame.operation == detail::Reply) {
                result = complete(frame);
            }

            if (!result) {
                return std::unexpected(result.error());
            }
        }
    } catch (...) {
        return std::unexpected(Error{ErrorCode::CallbackFailed, ErrorOperation::Pump});
    }

    const auto after = this->stats();
    stats.requests = after.requests - before.requests;
    stats.completions = after.completions - before.completions;
    return stats;
}
}
