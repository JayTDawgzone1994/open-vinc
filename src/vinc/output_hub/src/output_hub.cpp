#include <vinc/time.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#ifdef VINC_OUTPUT_HUB_TESTING
#include "output_hub_test_hooks.hpp"
#endif

namespace vinc::output_hub {
using namespace std::chrono_literals;

namespace detail {
struct Queue {
    std::mutex mutex;
    QueueLimits limits;
    std::vector<std::shared_ptr<const Event>> ring;
    std::size_t first = 0;
    std::size_t size = 0;
    std::size_t bytes = 0;
    bool enabled = true;
    Loss loss;

    explicit Queue(QueueLimits queue_limits) : limits(queue_limits), ring(queue_limits.events) {
    }

    // Caller holds mutex and the queue is nonempty.
    void pop(bool lost) {
        auto& item = ring[first];
        if (lost) {
            loss.bytes += item->bytes.size();
            ++loss.events;
        }
        bytes -= item->bytes.size();
        item.reset();
        first = (first + 1) % ring.size();
        --size;
    }

    void push(const std::shared_ptr<const Event>& event) {
        std::lock_guard lock(mutex);
        if (!enabled) {
            return;
        }

        while (size && (size == ring.size() || event->bytes.size() > limits.bytes - bytes)) {
            pop(true);
        }
        if (event->bytes.size() > limits.bytes) {
            loss.bytes += event->bytes.size();
            ++loss.events;
            return;
        }

        ring[(first + size) % ring.size()] = event;
        ++size;
        bytes += event->bytes.size();
    }

    void discard_for_gap(std::size_t missing_bytes) {
        std::lock_guard lock(mutex);
        if (!enabled) {
            return;
        }

        // Loss is delivered before the next batch, so no pre-gap event may
        // remain behind it. This cleanup must work without allocating.
        while (size) {
            pop(true);
        }
        loss.bytes += missing_bytes;
        ++loss.events;
    }
};
}

Batch Subscription::take(std::size_t maximum) {
    Batch batch;
    if (!queue_ || !maximum) {
        return batch;
    }

    std::lock_guard lock(queue_->mutex);
    const auto count = std::min(maximum, queue_->size);
    batch.events.reserve(count); // do not change queue/loss if allocation fails
    batch.skipped = std::exchange(queue_->loss, {});
    for (std::size_t i = 0; i < count; ++i) {
        batch.events.push_back(queue_->ring[queue_->first]);
        queue_->pop(false);
    }
    return batch;
}

void Subscription::set_enabled(bool enabled) {
    if (!queue_) {
        return;
    }

    std::lock_guard lock(queue_->mutex);
    if (queue_->enabled == enabled) {
        return;
    }

    queue_->enabled = enabled;
    while (queue_->size) {
        queue_->pop(true);
    }
    // A discontinuity also invalidates a destination's unfinished text fragment.
    ++queue_->loss.events;
}

namespace {
// Fixed stream mapping used by source IDs and reader ownership.
constexpr std::size_t captured_stream_count = 2;
constexpr std::size_t stdout_index = 0;
constexpr std::size_t stderr_index = 1;

vinc::Error error(ErrorCode code, ErrorOperation operation) {
    return {code, operation};
}

// Preserve native diagnostics while translating the public failure into this layer.
vinc::Error read_failure(const vinc::Error& cause) {
    return vinc::wrap_error(ErrorCode::ReadFailed, ErrorOperation::ReadOutput, cause);
}

}

struct OutputHub::Impl {
    struct Reader {
        std::shared_ptr<const Source> source;
        osal::PipeReader pipe;
        osal::IoBuffer buffer{config::output_hub_read_chunk_bytes};
        std::optional<osal::IoId> pending;
        bool done = false;
        bool closing = false;
    };

    struct Capture {
        std::uint64_t id = 0;
        std::array<Reader, captured_stream_count> readers;
        bool starting = true;
        vinc::Deadline deadline = vinc::Deadline::max();
    };

    Limits limits;
    osal::PipeService pipes;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Capture>> captures;
    std::vector<std::weak_ptr<detail::Queue>> queues;
    std::uint64_t next_id = 1;
    std::uint64_t sequence = 0;
    Stats counters;
    std::optional<vinc::Error> service_failure;
    std::jthread worker;

    // Member RAII releases the service and reservations if construction fails.
    // The worker starts only after both vectors have reserved their capacity.
    static vinc::Result<osal::PipeService> create_pipe_service(const Limits& limits) {
#ifdef VINC_OUTPUT_HUB_TESTING
        test::checkpoint(test::Point::CreateService);
#endif
        const auto reader_count = limits.max_captures * captured_stream_count;
        return osal::PipeService::create({reader_count, reader_count,
                                          reader_count * config::output_hub_read_chunk_bytes,
                                          config::output_hub_read_chunk_bytes});
    }

    void reserve_tracking_storage() {
        captures.reserve(limits.max_captures);
        queues.reserve(limits.max_subscribers);
    }

    void start_collection_worker() {
        worker = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }

    Impl(Limits configured_limits, osal::PipeService service)
        : limits(configured_limits), pipes(std::move(service)) {
        reserve_tracking_storage();
    }

    // Caller holds mutex. Keep the first infrastructure fault independently
    // of disposable events, including when there are no active captures.
    void observe_service_failure() {
        if (service_failure) {
            return;
        }

        auto cause = pipes.monitor_error();
#ifdef VINC_OUTPUT_HUB_TESTING
        cause = test::monitor_error(cause);
#endif
        if (cause) {
            service_failure = vinc::wrap_error(ErrorCode::ServiceFailed, ErrorOperation::MonitorOutput, *cause);
        }
    }

    vinc::Error capture_failure(const vinc::Error& fallback) {
        observe_service_failure();
        const bool service_closed = fallback.is(osal::ErrorCode::ServiceClosed) ||
                                    fallback.is(osal::ErrorCode::Closed) ||
                                    fallback.is(osal::ErrorCode::Cancelled);
        if (service_failure && service_closed) {
            return read_failure(*service_failure);
        }

        return read_failure(fallback);
    }

    // Caller holds mutex. Event creation may allocate; insertion into each fixed
    // queue does not. Lock order is hub mutex, then queue mutex; consumers only
    // acquire their queue mutex. No destination I/O or user callback runs here.
    void publish(Reader& reader, EventKind kind, std::string_view bytes = {},
                 std::optional<vinc::Error> os_error = {}) {
        try {
#ifdef VINC_OUTPUT_HUB_TESTING
            test::checkpoint(test::Point::Publish);
#endif
            auto event =
                std::make_shared<Event>(Event{reader.source, ++sequence, std::chrono::steady_clock::now(),
                                              kind, std::string(bytes), os_error});
            for (auto& weak_queue : queues) {
                if (auto queue = weak_queue.lock()) {
                    queue->push(event);
                }
            }
        } catch (const std::bad_alloc&) {
            // Allocation failure must not stop draining child pipes or recurse
            // through this same output route to report itself.
            for (auto& weak_queue : queues) {
                if (auto queue = weak_queue.lock()) {
                    queue->discard_for_gap(bytes.size());
                }
            }
        }
    }

    void finish(Reader& reader, EventKind kind, std::optional<vinc::Error> os_error = {}) {
        if (reader.done) {
            return;
        }

        reader.done = true;
        reader.pipe.close();
        if (kind == EventKind::Truncated) {
            ++counters.truncated_streams;
        }
        if (kind == EventKind::Failed) {
            ++counters.failed_streams;
        }
        publish(reader, kind, {}, os_error);
    }

    // Caller holds mutex throughout reader state changes and completion dispatch.
    void submit_reader(Reader& reader) {
        try {
            auto submitted = pipes.submit_read(reader.pipe, std::move(reader.buffer), vinc::Deadline::max());
            if (submitted) {
                reader.pending = *submitted;
            } else {
                finish(reader, EventKind::Failed, capture_failure(submitted.error()));
            }
        } catch (const std::bad_alloc&) {
            finish(reader, EventKind::Failed,
                   error(ErrorCode::ResourceExhausted, ErrorOperation::ReadOutput));
        }
    }

    void advance_captures(vinc::Deadline now) {
        for (auto& capture : captures) {
            // Spawn publishes the readers before clearing starting under this lock.
            if (capture->starting) {
                continue;
            }

            for (auto& reader : capture->readers) {
                if (reader.done) {
                    continue;
                }

                if (now >= capture->deadline) {
                    reader.closing = true;
                    reader.pipe.close();
                    // Pending I/O still owns its buffer; finish after its completion.
                    if (!reader.pending) {
                        finish(reader, EventKind::Truncated);
                    }
                    continue;
                }

                if (!reader.pending) {
                    submit_reader(reader);
                }
            }
        }

        std::erase_if(captures, [](const auto& capture) {
            return !capture->starting && capture->readers[stdout_index].done &&
                   capture->readers[stderr_index].done;
        });
        changed.notify_all();
    }

    void fail_active_captures(const vinc::Error& failure) {
        for (auto& capture : captures) {
            if (!capture->starting) {
                for (auto& reader : capture->readers) {
                    finish(reader, EventKind::Failed, capture_failure(failure));
                }
            }
        }
    }

    void complete_reader(Reader& reader, osal::IoCompletion& completion) {
        reader.pending.reset();
        if (completion.transferred) {
            counters.received_bytes += completion.transferred;
            // OSAL owns a contiguous byte buffer and bounds transferred by its size.
            // char may alias object bytes without extra alignment. publish() copies
            // this view before the completion buffer is moved back to the reader.
            const auto* data = reinterpret_cast<const char*>(completion.buffer.bytes().data());
            publish(reader, EventKind::Data, {data, completion.transferred});
        }
        reader.buffer = std::move(completion.buffer);

        // Preserve EOF precedence: a completed drain wins over a concurrent deadline.
        if (completion.status == osal::IoStatus::EndOfStream) {
            finish(reader, EventKind::End);
        } else if (reader.closing) {
            finish(reader, EventKind::Truncated);
        } else if (completion.status != osal::IoStatus::Completed) {
            observe_service_failure();
            auto failure = error(ErrorCode::ReadFailed, ErrorOperation::ReadOutput);
            const bool service_closed = completion.status == osal::IoStatus::Closed ||
                                        completion.status == osal::IoStatus::Cancelled;
            if (service_failure && service_closed) {
                failure = read_failure(*service_failure);
            } else if (completion.error) {
                // An independent native read failure is not caused by the monitor.
                failure = read_failure(*completion.error);
            }
            finish(reader, EventKind::Failed, failure);
        }
    }

    void dispatch_completion(osal::IoCompletion& completion) {
        for (auto& capture : captures) {
            for (auto& reader : capture->readers) {
                if (!reader.pending || *reader.pending != completion.id) {
                    continue;
                }

                complete_reader(reader, completion);
            }
        }
    }

    // Capture allocation owns both buffers locally until admission succeeds.
    static std::shared_ptr<Capture> allocate_capture() {
        return std::make_shared<Capture>();
    }

    // Caller holds mutex. On failure, capture retains any allocated sources and
    // remains unregistered; consumed IDs are deliberately not reused.
    vinc::Result<void> register_capture(const std::shared_ptr<Capture>& capture, const std::string& name,
                                        std::uint64_t generation) {
        observe_service_failure();
        if (service_failure) {
            return std::unexpected(vinc::wrap_error(ErrorCode::SpawnFailed, ErrorOperation::Spawn,
                                                  *service_failure));
        }

        if (captures.size() == limits.max_captures || next_id > UINT64_MAX / captured_stream_count) {
            return std::unexpected(error(ErrorCode::ResourceExhausted, ErrorOperation::Spawn));
        }

        capture->id = next_id++;
        for (std::size_t i = 0; i < captured_stream_count; ++i) {
            capture->readers[i].source =
                std::make_shared<Source>(Source{capture->id * captured_stream_count + i, generation, name,
                                                i == stdout_index ? Stream::Stdout : Stream::Stderr});
        }
        captures.push_back(capture);
        return {};
    }

    // Caller holds mutex. Both readers move into capture before it becomes visible
    // to the worker; the returned Process remains owned by the spawn caller.
    static CapturedProcess attach_readers(Capture& capture, osal::ProcessWithOutput& child) {
        capture.readers[stdout_index].pipe = std::move(*child.stdout_reader);
        capture.readers[stderr_index].pipe = std::move(*child.stderr_reader);
        capture.starting = false;
        return CapturedProcess{std::move(child.process), capture.id};
    }

    // Caller holds mutex. The spawn scope keeps capture alive until rollback ends.
    void rollback_capture(const std::shared_ptr<Capture>& capture) {
        std::erase(captures, capture);
        changed.notify_all();
    }

    void run(std::stop_token stop) {
        std::array<osal::IoCompletion, config::output_hub_completion_batch_size> completions;
        while (!stop.stop_requested()) {
            {
                std::lock_guard lock(mutex);
                observe_service_failure();
                advance_captures(std::chrono::steady_clock::now());
            }

            // Wait outside the hub lock so admission and subscriptions can progress.
            auto count = pipes.wait_completions(completions, std::chrono::steady_clock::now() +
                                                                 config::output_hub_poll_interval);
            if (!count) {
                if (!(count.error().is(vinc::osal::ErrorCode::Timeout))) {
                    {
                        std::lock_guard lock(mutex);
                        observe_service_failure();
                        fail_active_captures(count.error());
                    }
                    // A closed/failed service must not turn the worker into a busy loop.
                    std::this_thread::sleep_for(config::output_hub_failure_backoff);
                }
                continue;
            }

            // Keep one lock across the entire batch, preserving event ordering.
            std::lock_guard lock(mutex);
            observe_service_failure();
            for (std::size_t i = 0; i < *count; ++i) {
                dispatch_completion(completions[i]);
            }
        }
    }
};

OutputHub::OutputHub(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {
}

vinc::Result<std::unique_ptr<OutputHub>> OutputHub::create(Limits limits) {
    const bool invalid_capture_limit =
        !limits.max_captures || limits.max_captures > config::output_hub_max_captures;
    const bool invalid_subscriber_limit =
        !limits.max_subscribers || limits.max_subscribers > config::output_hub_max_subscribers;
    const bool empty_queue_budget = !limits.total_queue_bytes || !limits.total_queue_events;
    const bool invalid_drain_timeout =
        limits.drain_timeout < 0ms || limits.drain_timeout > config::output_hub_max_drain_timeout;
    if (invalid_capture_limit || invalid_subscriber_limit || empty_queue_budget || invalid_drain_timeout) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Create));
    }

    try {
        auto service = Impl::create_pipe_service(limits);
        if (!service) {
            return std::unexpected(
                vinc::wrap_error(ErrorCode::ServiceFailed, ErrorOperation::Create, service.error()));
        }
#ifdef VINC_OUTPUT_HUB_TESTING
        test::checkpoint(test::Point::ServiceCreated);
#endif

        auto impl = std::make_unique<Impl>(limits, std::move(*service));
        auto hub = std::unique_ptr<OutputHub>(new OutputHub(std::move(impl)));
        hub->impl_->start_collection_worker();
        return hub;
    } catch (const std::system_error& failure) {
        return std::unexpected(vinc::Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker,
                                           osal::native_error(failure.code())});
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(ErrorCode::ResourceExhausted, ErrorOperation::Create));
    }
}

OutputHub::~OutputHub() {
    impl_->worker.request_stop();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    (void)impl_->pipes.shutdown_until(std::chrono::steady_clock::now() + config::output_hub_shutdown_timeout);
}

vinc::Result<Subscription> OutputHub::subscribe(QueueLimits limits) {
    if (!limits.bytes || !limits.events) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Subscribe));
    }

    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->queues, [](const auto& queue) {
        return queue.expired();
    });
    auto remaining_bytes = impl_->limits.total_queue_bytes;
    auto remaining_events = impl_->limits.total_queue_events;
    for (auto& weak_queue : impl_->queues) {
        if (auto queue = weak_queue.lock()) {
            remaining_bytes -= queue->limits.bytes;
            remaining_events -= queue->limits.events;
        }
    }
    if (impl_->queues.size() == impl_->limits.max_subscribers || limits.bytes > remaining_bytes ||
        limits.events > remaining_events) {
        return std::unexpected(error(ErrorCode::ResourceExhausted, ErrorOperation::Subscribe));
    }

    auto queue = std::make_shared<detail::Queue>(limits);
    impl_->queues.push_back(queue);
    return Subscription(std::move(queue));
}

vinc::Result<CapturedProcess> OutputHub::spawn(osal::ProcessService& processes,
                                               const osal::SpawnOptions& options, std::string name,
                                               std::uint64_t generation) {
    if (name.empty() || name.size() > config::output_hub_max_source_name_bytes) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Spawn));
    }

    auto capture = Impl::allocate_capture();
    auto launch = options;
    launch.stderr_mode = osal::StdioMode::Capture;
    launch.stdout_mode = osal::StdioMode::Capture;
    {
        std::lock_guard lock(impl_->mutex);
        auto registered = impl_->register_capture(capture, name, generation);
        if (!registered) {
            return std::unexpected(registered.error());
        }
    }

    // Admission stays reserved while OSAL starts the process outside the lock.
    // Both Result failure and exceptions remove it before leaving this scope.
    try {
        auto child = osal::spawn_with_output(processes, impl_->pipes, launch);
        std::lock_guard lock(impl_->mutex);
        if (!child) {
            impl_->rollback_capture(capture);
            impl_->observe_service_failure();
            auto cause = child.error();
            if (impl_->service_failure && cause.is(osal::ErrorCode::ServiceClosed)) {
                cause = *impl_->service_failure;
            }
            return std::unexpected(
                vinc::wrap_error(ErrorCode::SpawnFailed, ErrorOperation::Spawn, cause));
        }

        return Impl::attach_readers(*capture, *child);
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        impl_->rollback_capture(capture);
        throw;
    }
}

void OutputHub::exited(std::uint64_t capture) {
    std::lock_guard lock(impl_->mutex);
    for (auto& entry : impl_->captures) {
        if (entry->id == capture && entry->deadline == vinc::Deadline::max()) {
            entry->deadline = std::chrono::steady_clock::now() + impl_->limits.drain_timeout;
        }
    }
}

void OutputHub::set_name(std::uint64_t capture, const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    for (auto& entry : impl_->captures) {
        if (entry->id == capture) {
            std::array<std::shared_ptr<const Source>, captured_stream_count> sources;
            // Allocate both replacements before publishing any rename boundary.
            // Allocation failure leaves the current reader labels intact.
            for (std::size_t i = 0; i < captured_stream_count; ++i) {
                auto renamed_source = *entry->readers[i].source;
                renamed_source.name = name;
                sources[i] = std::make_shared<Source>(std::move(renamed_source));
            }
            for (std::size_t i = 0; i < captured_stream_count; ++i) {
                // Flush any old-label fragment before publishing under the new label.
                impl_->publish(entry->readers[i], EventKind::Renamed);
                entry->readers[i].source = std::move(sources[i]);
            }
            return;
        }
    }
}

vinc::Result<void> OutputHub::wait_until(vinc::Deadline deadline) {
    std::unique_lock lock(impl_->mutex);
    const bool drained = impl_->changed.wait_until(lock, deadline, [&] {
        return impl_->captures.empty();
    });
    if (!drained) {
        return std::unexpected(error(ErrorCode::Timeout, ErrorOperation::Wait));
    }

    return {};
}

Stats OutputHub::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->counters;
    result.active_captures = impl_->captures.size();
    return result;
}

std::optional<vinc::Error> OutputHub::service_error() const {
    std::lock_guard lock(impl_->mutex);
    impl_->observe_service_failure();
    return impl_->service_failure;
}
}
