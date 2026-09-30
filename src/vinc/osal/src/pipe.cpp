#include "pipe_state.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::osal::detail {
Result<std::uint64_t> pipe_identity(Operation operation) noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(OsError{ErrorCode::ResourceExhausted, operation});
    } while (!sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}
void request_shutdown(const std::shared_ptr<PipeState>& state, bool abandon) noexcept {
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        state->accepting = false;
        state->abandoned |= abandon;
        for (auto& pipe : state->pipes) pipe->closing = true;
        if (abandon) {
            std::erase_if(state->operations, [&](const auto& op) {
                if (!op->done) return false;
                state->buffered_bytes -= op->buffer.capacity();
                return true;
            });
        }
    }
    state->monitor->wake();
}
void finish(PipeOperationState& op, NativeIoResult result) noexcept {
    // A completed transfer wins a race with cancellation. Only an actual
    // cancelled native operation is translated to the requested stop reason.
    if (result.status == IoStatus::Cancelled && op.stop_reason) result.status = *op.stop_reason;
    op.result = result;
    op.done = true;
}
void pipe_worker(const std::shared_ptr<PipeState>& state) noexcept {
    auto initialized = state->monitor->initialize_worker();
    if (!initialized) {
        { std::lock_guard lock(state->mutex); state->monitor_error = initialized.error(); }
        request_shutdown(state, false);
    }
    for (;;) {
        int timeout = -1;
        {
            std::lock_guard lock(state->mutex);
            const auto now = std::chrono::steady_clock::now();
            Deadline next = Deadline::max();
            bool pending = false;
            for (auto& entry : state->operations) {
                auto& op = *entry;
                if (op.done) continue;
                if (op.started) {
                    if (auto result = op.native.poll()) { finish(op, *result); continue; }
                }
                if (!op.stop_reason) {
                    if (op.pipe->closing) op.stop_reason = IoStatus::Closed;
                    else if (now >= op.deadline) op.stop_reason = IoStatus::TimedOut;
                }
                if (!op.started && op.stop_reason) {
                    finish(op, {*op.stop_reason});
                    continue;
                }
                if (!op.started) {
                    op.native.start(*op.pipe->native, *state->monitor, op.buffer.bytes(), op.kind);
                    op.started = true;
                    if (auto result = op.native.poll()) { finish(op, *result); continue; }
                }
                if (op.stop_reason && !op.cancellation_sent) {
                    op.native.cancel();
                    op.cancellation_sent = true;
                    if (auto result = op.native.poll()) { finish(op, *result); continue; }
                }
                pending = true;
                if (!op.stop_reason) next = std::min(next, op.deadline);
            }
            bool starting = false;
            for (auto& pipe : state->pipes) {
                starting |= pipe->starting || pipe->transferring;
                if (pipe->closing && !pipe->starting && !pipe->transferring) {
                    const bool active = std::any_of(state->operations.begin(), state->operations.end(),
                        [&](const auto& op) { return op->pipe == pipe && !op->done; });
                    if (!active) pipe->native.reset();
                }
            }
            std::erase_if(state->pipes, [](const auto& pipe) { return pipe->closing && !pipe->starting && !pipe->transferring && !pipe->native; });
            if (state->abandoned) {
                std::erase_if(state->operations, [&](const auto& op) {
                    if (!op->done) return false;
                    state->buffered_bytes -= op->buffer.capacity();
                    return true;
                });
            }
            state->changed.notify_all();
            if (!state->accepting && !pending && !starting) {
                state->worker_done = true;
                state->changed.notify_all();
                return;
            }
            if (next != Deadline::max()) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count();
                timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
            }
            if (state->monitor_error) timeout = 50;
        }
        auto result = state->monitor->wait(timeout);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->monitor_error = result.error(); }
            request_shutdown(state, false);
            // Never release live native I/O storage merely because its event
            // infrastructure failed, and never spin on a persistent failure.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

Result<std::vector<std::shared_ptr<PipeRecord>>> PipeAccess::reserve(PipeService& service, std::size_t count) {
    if (!service.state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::CreatePipe});
    std::lock_guard lock(service.state_->mutex);
    if (!service.state_->accepting) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::CreatePipe});
    if (count > service.state_->options.max_endpoints - service.state_->pipes.size())
        return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::CreatePipe});
    std::vector<std::shared_ptr<PipeRecord>> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto id = pipe_identity(Operation::CreatePipe);
        if (!id) return std::unexpected(id.error());
        auto record = std::make_shared<PipeRecord>();
        record->id = PipeId{*id};
        records.push_back(std::move(record));
    }
    // All allocations precede publishing any capacity reservation.
    for (const auto& record : records) service.state_->pipes.push_back(record);
    return records;
}
void PipeAccess::rollback(PipeService& service, std::span<const std::shared_ptr<PipeRecord>> records) noexcept {
    { std::lock_guard lock(service.state_->mutex);
      for (const auto& record : records) std::erase(service.state_->pipes, record); }
    service.state_->monitor->wake();
}
PipeEnd PipeAccess::commit(PipeService& service, const std::shared_ptr<PipeRecord>& record,
    std::unique_ptr<NativePipe> native, IoKind direction) {
    { std::lock_guard lock(service.state_->mutex);
      record->native = std::move(native); record->direction = direction; record->starting = false; }
    service.state_->monitor->wake();
    return PipeEnd(service.state_, record);
}
void PipeAccess::discard(PipeEnd& endpoint) noexcept {
    if (!endpoint.record_) return;
    { std::lock_guard lock(endpoint.state_->mutex);
      endpoint.record_->closing = true;
      endpoint.record_->native.reset(); // Unpublished; no operations or handoffs.
      std::erase(endpoint.state_->pipes, endpoint.record_); }
    endpoint.state_->monitor->wake();
}
} // namespace vinc::osal::detail

namespace vinc::osal::detail {
PipeEnd::PipeEnd(std::shared_ptr<detail::PipeState> state, std::shared_ptr<detail::PipeRecord> record)
    : state_(std::move(state)), record_(std::move(record)) {}
PipeEnd::~PipeEnd() { close(); }
PipeEnd::PipeEnd(PipeEnd&&) noexcept = default;
PipeEnd& PipeEnd::operator=(PipeEnd&& other) noexcept {
    if (this != &other) { close(); state_ = std::move(other.state_); record_ = std::move(other.record_); }
    return *this;
}
PipeEnd::operator bool() const noexcept { return static_cast<bool>(record_); }
PipeId PipeEnd::id() const noexcept { return record_ ? record_->id : PipeId{}; }
void PipeEnd::close() noexcept {
    if (!record_) return;
    { std::lock_guard lock(state_->mutex); record_->closing = true; }
    state_->monitor->wake();
}
} // namespace vinc::osal::detail

namespace vinc::osal {
PipeService::PipeService(std::shared_ptr<detail::PipeState> state, std::unique_ptr<detail::PipeWorker> worker)
    : state_(std::move(state)), worker_(std::move(worker)) {}
PipeService::~PipeService() { close(); }
PipeService::PipeService(PipeService&&) noexcept = default;
PipeService& PipeService::operator=(PipeService&& other) noexcept {
    if (this != &other) { close(); worker_ = std::move(other.worker_); state_ = std::move(other.state_); }
    return *this;
}
void PipeService::close() noexcept { detail::request_shutdown(state_, true); }
Result<PipeService> PipeService::create(PipeServiceOptions options) {
    if (!options.max_endpoints || !options.max_operations || !options.max_buffered_bytes ||
        !options.max_transfer_bytes || options.max_transfer_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::InitializeService});
    auto monitor = detail::PipeMonitor::create();
    if (!monitor) return std::unexpected(monitor.error());
    auto state = std::make_shared<detail::PipeState>();
    state->options = options;
    state->monitor = std::move(*monitor);
    state->pipes.reserve(options.max_endpoints);
    state->operations.reserve(options.max_operations);
    auto worker = std::make_unique<detail::PipeWorker>();
    try { worker->thread = std::thread([state] { detail::pipe_worker(state); }); }
    catch (const std::system_error& error) {
        return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::InitializeService, error.code()});
    }
    return PipeService(std::move(state), std::move(worker));
}
Result<Pipe> PipeService::create_pipe() {
    auto records = detail::PipeAccess::reserve(*this, 2);
    if (!records) return std::unexpected(records.error());
    try {
        auto pair = detail::NativePipe::create_pair();
        if (!pair) { detail::PipeAccess::rollback(*this, *records); return std::unexpected(pair.error()); }
        Pipe result;
        result.reader.end_ = detail::PipeAccess::commit(*this, (*records)[0], std::move(pair->reader), IoKind::Read);
        result.writer.end_ = detail::PipeAccess::commit(*this, (*records)[1], std::move(pair->writer), IoKind::Write);
        return result;
    } catch (...) { detail::PipeAccess::rollback(*this, *records); throw; }
}
Result<IoId> PipeService::submit(detail::PipeEnd& pipe, IoBuffer&& buffer, Deadline deadline, IoKind kind) {
    const auto operation = kind == IoKind::Read ? Operation::ReadPipe : Operation::WritePipe;
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, operation});
    if (!pipe.record_ || pipe.state_ != state_ || buffer.size() == 0 || buffer.size() > state_->options.max_transfer_bytes)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting) return std::unexpected(OsError{ErrorCode::ServiceClosed, operation});
    if (pipe.record_->direction != kind) return std::unexpected(OsError{ErrorCode::InvalidArgument, operation});
    if (pipe.record_->transferring) return std::unexpected(OsError{ErrorCode::Busy, operation});
    if (pipe.record_->closing) return std::unexpected(OsError{ErrorCode::Closed, operation});
    for (const auto& op : state_->operations)
        if (op->pipe == pipe.record_ && op->kind == kind) return std::unexpected(OsError{ErrorCode::Busy, operation});
    if (state_->operations.size() == state_->options.max_operations ||
        buffer.capacity() > state_->options.max_buffered_bytes - state_->buffered_bytes)
        return std::unexpected(OsError{ErrorCode::ResourceExhausted, operation});
    auto id = detail::pipe_identity(operation);
    if (!id) return std::unexpected(id.error());
    auto op = std::make_unique<detail::PipeOperationState>();
    op->id = IoId{*id}; op->pipe = pipe.record_; op->kind = kind; op->deadline = deadline;
    auto attached = pipe.record_->native->attach(*state_->monitor);
    if (!attached) return std::unexpected(attached.error());
    pipe.record_->used = true;
    op->buffer = std::move(buffer); // all fallible allocations have completed
    state_->buffered_bytes += op->buffer.capacity();
    state_->operations.push_back(std::move(op));
    state_->monitor->wake();
    return IoId{*id};
}
Result<IoId> PipeService::submit_read(PipeReader& pipe, IoBuffer&& buffer, Deadline deadline) {
    return submit(pipe.end_, std::move(buffer), deadline, IoKind::Read);
}
Result<IoId> PipeService::submit_write(PipeWriter& pipe, IoBuffer&& buffer, Deadline deadline) {
    return submit(pipe.end_, std::move(buffer), deadline, IoKind::Write);
}
Result<CancelResult> PipeService::cancel(IoId id) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::CancelIo});
    std::lock_guard lock(state_->mutex);
    for (auto& op : state_->operations) if (op->id == id) {
        if (op->done) return CancelResult::AlreadyCompleted;
        if (!op->stop_reason) op->stop_reason = IoStatus::Cancelled;
        state_->monitor->wake();
        return CancelResult::Requested;
    }
    return std::unexpected(OsError{ErrorCode::NotFound, Operation::CancelIo});
}
Result<std::size_t> PipeService::wait_completions(std::span<IoCompletion> output, Deadline deadline, std::stop_token token) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::WaitIo});
    if (output.empty()) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::WaitIo});
    std::unique_lock lock(state_->mutex);
    const auto ready = [&] { return state_->worker_done || std::any_of(state_->operations.begin(), state_->operations.end(), [](const auto& op) { return op->done; }); };
    if (!state_->changed.wait_until(lock, token, deadline, ready))
        return std::unexpected(OsError{token.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout, Operation::WaitIo});
    std::size_t count = 0;
    for (auto it = state_->operations.begin(); it != state_->operations.end() && count < output.size();) {
        auto& op = **it;
        if (!op.done) { ++it; continue; }
        state_->buffered_bytes -= op.buffer.capacity();
        output[count++] = IoCompletion{op.id, op.pipe->id, op.kind, op.result.status,
            op.result.transferred, op.result.error, std::move(op.buffer)};
        it = state_->operations.erase(it);
    }
    if (count == 0) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::WaitIo});
    return count;
}
Result<void> PipeService::shutdown_until(Deadline deadline, std::stop_token token) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Shutdown});
    detail::request_shutdown(state_, false);
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_until(lock, token, deadline, [&] { return state_->worker_done; }))
        return std::unexpected(OsError{token.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout, Operation::Shutdown});
    auto error = state_->monitor_error;
    lock.unlock();
    { std::lock_guard join_lock(worker_->join_mutex); if (worker_->thread.joinable()) worker_->thread.join(); }
    if (error) return std::unexpected(*error);
    return {};
}
std::optional<OsError> PipeService::monitor_error() const {
    if (!state_) return std::nullopt;
    std::lock_guard lock(state_->mutex);
    return state_->monitor_error;
}
} // namespace vinc::osal
