#include <vinc/osal/detail/native_error.hpp>
#include <vinc/osal/detail/operation_error.hpp>
// Common Pipe scheduler: accept owned buffers, advance native I/O, then return completions.
// Endpoint close requests cancellation; native completion determines when storage is safe to free.

#include <vinc/time.hpp>
#include <vinc/osal/detail/pipe_state.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::osal::detail {
namespace {
// Native I/O still owns its buffers after monitor failure; poll without spinning.
constexpr auto monitor_failure_retry_interval = std::chrono::milliseconds(50);
}

vinc::Result<std::uint64_t> pipe_identity(vinc::osal::ErrorOperation operation) noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(vinc::Error{vinc::osal::ErrorCode::ResourceExhausted, operation});
        }
    } while (!sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

void request_shutdown(const std::shared_ptr<PipeState>& state, bool abandon) noexcept {
    if (!state) {
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->accepting = false;
        state->abandoned |= abandon;
        for (auto& pipe : state->pipes) {
            pipe->closing = true;
        }

        if (abandon) {
            std::erase_if(state->operations, [&](const auto& operation) {
                if (!operation->done) {
                    return false;
                }
                state->buffered_bytes -= operation->buffer.capacity();
                return true;
            });
        }
    }
    state->monitor->wake();
}

void finish(PipeOperationState& operation, NativeIoResult result) noexcept {
    // A completed transfer wins a race with cancellation. Only an actual
    // cancelled native operation is translated to the requested stop reason.
    if (result.status == IoStatus::Cancelled && operation.stop_reason) {
        result.status = *operation.stop_reason;
    }
    operation.result = result;
    operation.done = true;
}

// Only this worker starts, polls and cancels native transfers. Completed buffers
// remain charged to the service budget until collected or explicitly abandoned.
void pipe_worker(const std::shared_ptr<PipeState>& state) noexcept {
    auto initialized = state->monitor->initialize_worker();
    if (!initialized) {
        {
            std::lock_guard lock(state->mutex);
            state->monitor_error = initialized.error();
        }
        request_shutdown(state, false);
    }

    for (;;) {
        int timeout = -1;
        {
            std::lock_guard lock(state->mutex);
            const auto now = std::chrono::steady_clock::now();
            vinc::Deadline next = vinc::Deadline::max();
            bool pending = false;
            for (auto& entry : state->operations) {
                auto& operation = *entry;
                if (operation.done) {
                    continue;
                }

                if (operation.started) {
                    if (auto result = operation.native.poll()) {
                        finish(operation, *result);
                        continue;
                    }
                }

                if (!operation.stop_reason) {
                    if (operation.pipe->closing) {
                        operation.stop_reason = IoStatus::Closed;
                    } else if (now >= operation.deadline) {
                        operation.stop_reason = IoStatus::TimedOut;
                    }
                }

                if (!operation.started && operation.stop_reason) {
                    finish(operation, {*operation.stop_reason});
                    continue;
                }

                if (!operation.started) {
                    operation.native.start(*operation.pipe->native, *state->monitor, operation.buffer.bytes(),
                                           operation.kind);
                    operation.started = true;
                    if (auto result = operation.native.poll()) {
                        finish(operation, *result);
                        continue;
                    }
                }

                if (operation.stop_reason && !operation.cancellation_sent) {
                    operation.native.cancel();
                    operation.cancellation_sent = true;
                    if (auto result = operation.native.poll()) {
                        finish(operation, *result);
                        continue;
                    }
                }
                pending = true;
                if (!operation.stop_reason) {
                    next = std::min(next, operation.deadline);
                }
            }

            // Reclaim only endpoints with no live or transferring operation.
            bool starting = false;
            for (auto& pipe : state->pipes) {
                starting |= pipe->starting || pipe->transferring;
                if (pipe->closing && !pipe->starting && !pipe->transferring) {
                    const bool active = std::any_of(state->operations.begin(), state->operations.end(),
                                                    [&](const auto& operation) {
                                                        return operation->pipe == pipe && !operation->done;
                                                    });
                    if (!active) {
                        pipe->native.reset();
                    }
                }
            }

            std::erase_if(state->pipes, [](const auto& pipe) {
                return pipe->closing && !pipe->starting && !pipe->transferring && !pipe->native;
            });
            if (state->abandoned) {
                std::erase_if(state->operations, [&](const auto& operation) {
                    if (!operation->done) {
                        return false;
                    }
                    state->buffered_bytes -= operation->buffer.capacity();
                    return true;
                });
            }

            state->changed.notify_all();
            if (!state->accepting && !pending && !starting) {
                state->worker_done = true;
                state->changed.notify_all();
                return;
            }

            if (next != vinc::Deadline::max()) {
                const auto remaining =
                    std::chrono::ceil<std::chrono::milliseconds>(next - std::chrono::steady_clock::now())
                        .count();
                timeout =
                    static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
            }

            if (state->monitor_error) {
                timeout = static_cast<int>(monitor_failure_retry_interval.count());
            }
        }

        auto result = state->monitor->wait(timeout);
        if (!result) {
            {
                std::lock_guard lock(state->mutex);
                state->monitor_error = result.error();
            }
            request_shutdown(state, false);
            // Never release live native I/O storage merely because its event
            // infrastructure failed, and never spin on a persistent failure.
            std::this_thread::sleep_for(monitor_failure_retry_interval);
        }
    }
}

vinc::Result<std::vector<std::shared_ptr<PipeRecord>>> PipeAccess::reserve(PipeService& service,
                                                                           std::size_t count) {
    if (!service.state_) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::CreatePipe});
    }

    std::lock_guard lock(service.state_->mutex);
    if (!service.state_->accepting) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::CreatePipe});
    }

    if (count > service.state_->options.max_endpoints - service.state_->pipes.size()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ResourceExhausted, vinc::osal::ErrorOperation::CreatePipe});
    }

    std::vector<std::shared_ptr<PipeRecord>> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto id = pipe_identity(vinc::osal::ErrorOperation::CreatePipe);
        if (!id) {
            return std::unexpected(id.error());
        }

        auto record = std::make_shared<PipeRecord>();
        record->id = PipeId{*id};
        records.push_back(std::move(record));
    }
    // All allocations precede publishing any capacity reservation.
    for (const auto& record : records) {
        service.state_->pipes.push_back(record);
    }

    return records;
}

void PipeAccess::rollback(PipeService& service,
                          std::span<const std::shared_ptr<PipeRecord>> records) noexcept {
    {
        std::lock_guard lock(service.state_->mutex);
        for (const auto& record : records) {
            std::erase(service.state_->pipes, record);
        }
    }
    service.state_->monitor->wake();
}

PipeEnd PipeAccess::commit(PipeService& service, const std::shared_ptr<PipeRecord>& record,
                           std::unique_ptr<NativePipe> native, IoKind direction) {
    {
        std::lock_guard lock(service.state_->mutex);
        record->native = std::move(native);
        record->direction = direction;
        record->starting = false;
    }
    service.state_->monitor->wake();
    return PipeEnd(service.state_, record);
}

void PipeAccess::discard(PipeEnd& endpoint) noexcept {
    if (!endpoint.record_) {
        return;
    }
    {
        std::lock_guard lock(endpoint.state_->mutex);
        endpoint.record_->closing = true;
        endpoint.record_->native.reset(); // Unpublished; no operations or handoffs.
        std::erase(endpoint.state_->pipes, endpoint.record_);
    }
    endpoint.state_->monitor->wake();
}
} // namespace vinc::osal::detail

namespace vinc::osal::detail {
PipeEnd::PipeEnd(std::shared_ptr<detail::PipeState> state, std::shared_ptr<detail::PipeRecord> record)
    : state_(std::move(state)), record_(std::move(record)) {
}

PipeEnd::~PipeEnd() {
    close();
}

PipeEnd::PipeEnd(PipeEnd&&) noexcept = default;

PipeEnd& PipeEnd::operator=(PipeEnd&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        record_ = std::move(other.record_);
    }

    return *this;
}

PipeEnd::operator bool() const noexcept {
    return static_cast<bool>(record_);
}

PipeId PipeEnd::id() const noexcept {
    return record_ ? record_->id : PipeId{};
}

void PipeEnd::close() noexcept {
    if (!record_) {
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        record_->closing = true;
    }
    state_->monitor->wake();
}
} // namespace vinc::osal::detail

namespace vinc::osal {
PipeService::PipeService(std::shared_ptr<detail::PipeState> state, std::unique_ptr<detail::PipeWorker> worker)
    : state_(std::move(state)), worker_(std::move(worker)) {
}

PipeService::~PipeService() {
    close();
}

PipeService::PipeService(PipeService&&) noexcept = default;

PipeService& PipeService::operator=(PipeService&& other) noexcept {
    if (this != &other) {
        close();
        worker_ = std::move(other.worker_);
        state_ = std::move(other.state_);
    }

    return *this;
}

void PipeService::close() noexcept {
    detail::request_shutdown(state_, true);
}

vinc::Result<PipeService> PipeService::create(PipeServiceOptions options) {
    if (!options.max_endpoints || !options.max_operations || !options.max_buffered_bytes ||
        !options.max_transfer_bytes ||
        options.max_transfer_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument,
                                           vinc::osal::ErrorOperation::InitializeService});
    }

    auto monitor = detail::PipeMonitor::create();
    if (!monitor) {
        return std::unexpected(monitor.error());
    }

    auto state = std::make_shared<detail::PipeState>();
    state->options = options;
    state->monitor = std::move(*monitor);
    state->pipes.reserve(options.max_endpoints);
    state->operations.reserve(options.max_operations);
    auto worker = std::make_unique<detail::PipeWorker>();
    try {
        worker->thread = std::thread([state] {
            detail::pipe_worker(state);
        });
    } catch (const std::system_error& error) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::ResourceExhausted,
                                           vinc::osal::ErrorOperation::InitializeService,
                                           detail::native_error(error.code())});
    }

    return PipeService(std::move(state), std::move(worker));
}

vinc::Result<Pipe> PipeService::create_pipe() {
    auto records = detail::PipeAccess::reserve(*this, 2);
    if (!records) {
        return std::unexpected(records.error());
    }
    try {
        auto pair = detail::NativePipe::create_pair();
        if (!pair) {
            detail::PipeAccess::rollback(*this, *records);
            return std::unexpected(pair.error());
        }
        Pipe result;
        result.reader.end_ =
            detail::PipeAccess::commit(*this, (*records)[0], std::move(pair->reader), IoKind::Read);
        result.writer.end_ =
            detail::PipeAccess::commit(*this, (*records)[1], std::move(pair->writer), IoKind::Write);
        return result;
    } catch (...) {
        detail::PipeAccess::rollback(*this, *records);
        throw;
    }
}

// Admission checks precede ownership transfer: rejection leaves the caller buffer intact.
// An uncollected completion still occupies this endpoint and its resource credits.
vinc::Result<IoId> PipeService::submit(detail::PipeEnd& pipe, IoBuffer&& buffer, vinc::Deadline deadline,
                                       IoKind kind) {
    const auto operation_code =
        kind == IoKind::Read ? vinc::osal::ErrorOperation::ReadPipe : vinc::osal::ErrorOperation::WritePipe;
    if (!state_) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::ServiceClosed, operation_code});
    }

    if (!pipe.record_ || pipe.state_ != state_ || buffer.size() == 0 ||
        buffer.size() > state_->options.max_transfer_bytes) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation_code});
    }

    std::lock_guard lock(state_->mutex);
    if (!state_->accepting) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::ServiceClosed, operation_code});
    }

    if (pipe.record_->direction != kind) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::InvalidArgument, operation_code});
    }

    if (pipe.record_->transferring) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Busy, operation_code});
    }

    if (pipe.record_->closing) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Closed, operation_code});
    }

    for (const auto& operation : state_->operations) {
        if (operation->pipe == pipe.record_ && operation->kind == kind) {
            return std::unexpected(vinc::Error{vinc::osal::ErrorCode::Busy, operation_code});
        }
    }

    if (state_->operations.size() == state_->options.max_operations ||
        buffer.capacity() > state_->options.max_buffered_bytes - state_->buffered_bytes) {
        return std::unexpected(vinc::Error{vinc::osal::ErrorCode::ResourceExhausted, operation_code});
    }

    auto id = detail::pipe_identity(operation_code);
    if (!id) {
        return std::unexpected(id.error());
    }

    auto operation = std::make_unique<detail::PipeOperationState>();
    operation->id = IoId{*id};
    operation->pipe = pipe.record_;
    operation->kind = kind;
    operation->deadline = deadline;
    auto attached = pipe.record_->native->attach(*state_->monitor);
    if (!attached) {
        return std::unexpected(detail::operation_error(operation_code, attached.error()));
    }
    pipe.record_->used = true;
    operation->buffer = std::move(buffer); // all fallible allocations have completed
    state_->buffered_bytes += operation->buffer.capacity();
    state_->operations.push_back(std::move(operation));
    state_->monitor->wake();
    return IoId{*id};
}

vinc::Result<IoId> PipeService::submit_read(PipeReader& pipe, IoBuffer&& buffer, vinc::Deadline deadline) {
    return submit(pipe.end_, std::move(buffer), deadline, IoKind::Read);
}

vinc::Result<IoId> PipeService::submit_write(PipeWriter& pipe, IoBuffer&& buffer, vinc::Deadline deadline) {
    return submit(pipe.end_, std::move(buffer), deadline, IoKind::Write);
}

vinc::Result<CancelResult> PipeService::cancel(IoId id) {
    if (!state_) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::CancelIo});
    }

    std::lock_guard lock(state_->mutex);
    for (auto& operation : state_->operations) {
        if (operation->id == id) {
            if (operation->done) {
                return CancelResult::AlreadyCompleted;
            }

            if (!operation->stop_reason) {
                operation->stop_reason = IoStatus::Cancelled;
            }
            state_->monitor->wake();
            return CancelResult::Requested;
        }
    }

    return std::unexpected(
        vinc::Error{vinc::osal::ErrorCode::NotFound, vinc::osal::ErrorOperation::CancelIo});
}

vinc::Result<std::size_t> PipeService::wait_completions(std::span<IoCompletion> output,
                                                        vinc::Deadline deadline, std::stop_token token) {
    if (!state_) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::WaitIo});
    }

    if (output.empty()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::WaitIo});
    }

    std::unique_lock lock(state_->mutex);
    const auto ready = [&] {
        return state_->worker_done ||
               std::any_of(state_->operations.begin(), state_->operations.end(), [](const auto& operation) {
                   return operation->done;
               });
    };
    if (!state_->changed.wait_until(lock, token, deadline, ready)) {
        return std::unexpected(vinc::Error{token.stop_requested() ? vinc::osal::ErrorCode::Cancelled
                                                                  : vinc::osal::ErrorCode::Timeout,
                                           vinc::osal::ErrorOperation::WaitIo});
    }

    std::size_t count = 0;
    for (auto it = state_->operations.begin(); it != state_->operations.end() && count < output.size();) {
        auto& operation = **it;
        if (!operation.done) {
            ++it;
            continue;
        }
        state_->buffered_bytes -= operation.buffer.capacity();
        output[count++] = IoCompletion{operation.id,
                                       operation.pipe->id,
                                       operation.kind,
                                       operation.result.status,
                                       operation.result.transferred,
                                       operation.result.error,
                                       std::move(operation.buffer)};
        it = state_->operations.erase(it);
    }

    if (count == 0) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::WaitIo});
    }

    return count;
}

vinc::Result<void> PipeService::shutdown_until(vinc::Deadline deadline, std::stop_token token) {
    if (!state_) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::Shutdown});
    }
    detail::request_shutdown(state_, false);
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_until(lock, token, deadline, [&] {
            return state_->worker_done;
        })) {
        return std::unexpected(vinc::Error{token.stop_requested() ? vinc::osal::ErrorCode::Cancelled
                                                                  : vinc::osal::ErrorCode::Timeout,
                                           vinc::osal::ErrorOperation::Shutdown});
    }

    auto error = state_->monitor_error;
    lock.unlock();
    {
        std::lock_guard join_lock(worker_->join_mutex);
        if (worker_->thread.joinable()) {
            worker_->thread.join();
        }
    }

    if (error) {
        return std::unexpected(*error);
    }

    return {};
}

std::optional<vinc::Error> PipeService::monitor_error() const {
    if (!state_) {
        return std::nullopt;
    }

    std::lock_guard lock(state_->mutex);
    return state_->monitor_error;
}
} // namespace vinc::osal
