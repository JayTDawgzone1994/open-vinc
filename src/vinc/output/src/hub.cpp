#include <vinc/output/hub.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vinc::output {
using namespace std::chrono_literals;
namespace detail {
struct Queue {
    std::mutex mutex;
    QueueLimits limits;
    std::vector<std::shared_ptr<const Event>> ring;
    std::size_t first = 0, size = 0, bytes = 0;
    bool enabled = true;
    Loss loss;
    explicit Queue(QueueLimits l) : limits(l), ring(l.events) {}
    void pop(bool lost) {
        auto& item = ring[first];
        if (lost) { loss.bytes += item->bytes.size(); ++loss.events; }
        bytes -= item->bytes.size(); item.reset();
        first = (first + 1) % ring.size(); --size;
    }
    void push(const std::shared_ptr<const Event>& event) {
        std::lock_guard lock(mutex);
        if (!enabled) return;
        while (size && (size == ring.size() || event->bytes.size() > limits.bytes - bytes)) pop(true);
        if (event->bytes.size() > limits.bytes) { loss.bytes += event->bytes.size(); ++loss.events; return; }
        ring[(first + size) % ring.size()] = event;
        ++size; bytes += event->bytes.size();
    }
};
}
Batch Subscription::take(std::size_t maximum) {
    Batch batch;
    if (!queue_ || !maximum) return batch;
    std::lock_guard lock(queue_->mutex);
    const auto count = std::min(maximum, queue_->size);
    batch.events.reserve(count); // do not change queue/loss if allocation fails
    batch.skipped = std::exchange(queue_->loss, {});
    for (std::size_t i = 0; i < count; ++i) {
        batch.events.push_back(queue_->ring[queue_->first]); queue_->pop(false);
    }
    return batch;
}
void Subscription::set_enabled(bool enabled) {
    if (!queue_) return;
    std::lock_guard lock(queue_->mutex);
    if (queue_->enabled == enabled) return;
    queue_->enabled = enabled;
    while (queue_->size) queue_->pop(true);
    // A discontinuity also invalidates a destination's unfinished text fragment.
    ++queue_->loss.events;
}
namespace {
constexpr std::size_t chunk_size = 4096;
osal::OsError error(osal::ErrorCode code) { return {code, osal::Operation::ReadPipe}; }
template<class T> T checked(osal::Result<T> result) {
    if (!result) throw std::runtime_error("cannot initialize output PipeService");
    return std::move(*result);
}
}
struct OutputHub::Impl {
    struct Reader {
        std::shared_ptr<const Source> source;
        osal::PipeReader pipe;
        osal::IoBuffer buffer{chunk_size};
        std::optional<osal::IoId> pending;
        bool done = false, closing = false;
    };
    struct Capture {
        std::uint64_t id = 0;
        std::array<Reader, 2> readers;
        bool starting = true;
        osal::Deadline deadline = osal::Deadline::max();
    };
    Limits limits;
    osal::PipeService pipes;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Capture>> captures;
    std::vector<std::weak_ptr<detail::Queue>> queues;
    std::uint64_t next_id = 1, sequence = 0;
    Stats counters;
    std::jthread worker;
    explicit Impl(Limits l) : limits(l), pipes(checked(osal::PipeService::create(
        {l.max_captures * 2, l.max_captures * 2, l.max_captures * 2 * chunk_size, chunk_size}))) {
        captures.reserve(l.max_captures); queues.reserve(l.max_subscribers);
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    // Called under the hub lock; queues have fixed slots, so destination fanout
    // cannot allocate, perform I/O, or run user callbacks.
    void publish(Reader& reader, EventKind kind, std::string_view bytes = {}, std::optional<osal::OsError> os = {}) {
        try {
            auto event = std::make_shared<Event>(Event{reader.source, ++sequence,
                std::chrono::steady_clock::now(), kind, std::string(bytes), os});
            for (auto& weak : queues) if (auto queue = weak.lock()) queue->push(event);
        } catch (const std::bad_alloc&) {
            // Allocation failure must not stop draining child pipes or recurse
            // through this same output route to report itself.
            for (auto& weak : queues) if (auto queue = weak.lock()) {
                std::lock_guard lock(queue->mutex);
                if (queue->enabled) { queue->loss.bytes += bytes.size(); ++queue->loss.events; }
            }
        }
    }
    void finish(Reader& reader, EventKind kind, std::optional<osal::OsError> os = {}) {
        if (reader.done) return;
        reader.done = true; reader.pipe.close();
        if (kind == EventKind::Truncated) ++counters.truncated_streams;
        if (kind == EventKind::Failed) ++counters.failed_streams;
        publish(reader, kind, {}, os);
    }
    void run(std::stop_token stop) {
        std::array<osal::IoCompletion, 32> completions;
        while (!stop.stop_requested()) {
            {
                std::lock_guard lock(mutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto& capture : captures) {
                    if (capture->starting) continue;
                    for (auto& reader : capture->readers) {
                        if (reader.done) continue;
                        if (now >= capture->deadline) {
                            reader.closing = true; reader.pipe.close();
                            if (!reader.pending) finish(reader, EventKind::Truncated);
                            continue;
                        }
                        if (!reader.pending) {
                            try {
                                auto id = pipes.submit_read(reader.pipe, std::move(reader.buffer), osal::Deadline::max());
                                if (id) reader.pending = *id;
                                else finish(reader, EventKind::Failed, id.error());
                            } catch (const std::bad_alloc&) { finish(reader, EventKind::Failed, error(osal::ErrorCode::ResourceExhausted)); }
                        }
                    }
                }
                std::erase_if(captures, [](const auto& c) {
                    return !c->starting && c->readers[0].done && c->readers[1].done;
                });
                changed.notify_all();
            }
            auto count = pipes.wait_completions(completions, std::chrono::steady_clock::now() + 20ms);
            if (!count) {
                if (count.error().code != osal::ErrorCode::Timeout) {
                    {
                        std::lock_guard lock(mutex);
                        for (auto& capture : captures) if (!capture->starting)
                            for (auto& reader : capture->readers) finish(reader, EventKind::Failed, count.error());
                    }
                    std::this_thread::sleep_for(20ms); // no spin on a closed/failed service
                }
                continue;
            }
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < *count; ++i) {
                auto& completion = completions[i];
                for (auto& capture : captures) for (auto& reader : capture->readers) {
                    if (!reader.pending || *reader.pending != completion.id) continue;
                    reader.pending.reset();
                    if (completion.transferred) {
                        counters.received_bytes += completion.transferred;
                        publish(reader, EventKind::Data, {reinterpret_cast<const char*>(completion.buffer.bytes().data()), completion.transferred});
                    }
                    reader.buffer = std::move(completion.buffer);
                    if (completion.status == osal::IoStatus::EndOfStream) finish(reader, EventKind::End);
                    else if (reader.closing) finish(reader, EventKind::Truncated);
                    else if (completion.status != osal::IoStatus::Completed) finish(reader, EventKind::Failed, completion.error);
                }
            }
        }
    }
};
OutputHub::OutputHub(Limits limits) {
    if (!limits.max_captures || limits.max_captures > 1024 || !limits.max_subscribers || limits.max_subscribers > 64 ||
        !limits.total_queue_bytes || !limits.total_queue_events || limits.drain_timeout < 0ms || limits.drain_timeout > 60s)
        throw std::invalid_argument("invalid output hub limits");
    impl_ = std::make_unique<Impl>(limits);
}
OutputHub::~OutputHub() {
    impl_->worker.request_stop(); impl_->worker.join();
    (void)impl_->pipes.shutdown_until(std::chrono::steady_clock::now() + 2s);
}
osal::Result<Subscription> OutputHub::subscribe(QueueLimits limits) {
    if (!limits.bytes || !limits.events) return std::unexpected(error(osal::ErrorCode::InvalidArgument));
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->queues, [](const auto& q) { return q.expired(); });
    auto bytes = impl_->limits.total_queue_bytes, events = impl_->limits.total_queue_events;
    for (auto& weak : impl_->queues) if (auto q = weak.lock()) { bytes -= q->limits.bytes; events -= q->limits.events; }
    if (impl_->queues.size() == impl_->limits.max_subscribers || limits.bytes > bytes || limits.events > events)
        return std::unexpected(error(osal::ErrorCode::ResourceExhausted));
    auto queue = std::make_shared<detail::Queue>(limits);
    impl_->queues.push_back(queue);
    return Subscription(std::move(queue));
}
osal::Result<CapturedProcess> OutputHub::spawn(osal::ProcessService& processes, const osal::SpawnOptions& options,
    std::string name, std::uint64_t generation) {
    if (name.empty() || name.size() > 256) return std::unexpected(error(osal::ErrorCode::InvalidArgument));
    auto capture = std::make_shared<Impl::Capture>();
    auto launch = options;
    launch.stdout_mode = launch.stderr_mode = osal::StdioMode::Capture;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->captures.size() == impl_->limits.max_captures || impl_->next_id > UINT64_MAX / 2)
            return std::unexpected(error(osal::ErrorCode::ResourceExhausted));
        capture->id = impl_->next_id++;
        for (std::size_t i = 0; i < 2; ++i)
            capture->readers[i].source = std::make_shared<Source>(Source{capture->id * 2 + i,
                generation, name, i == 0 ? Stream::Stdout : Stream::Stderr});
        impl_->captures.push_back(capture);
    }
    try {
        auto child = osal::spawn_with_output(processes, impl_->pipes, launch);
        std::lock_guard lock(impl_->mutex);
        if (!child) {
            std::erase(impl_->captures, capture); impl_->changed.notify_all();
            return std::unexpected(child.error());
        }
        capture->readers[0].pipe = std::move(*child->stdout_reader);
        capture->readers[1].pipe = std::move(*child->stderr_reader);
        capture->starting = false;
        return CapturedProcess{std::move(child->process), capture->id};
    } catch (...) {
        std::lock_guard lock(impl_->mutex); std::erase(impl_->captures, capture); impl_->changed.notify_all(); throw;
    }
}
void OutputHub::exited(std::uint64_t capture) {
    std::lock_guard lock(impl_->mutex);
    for (auto& c : impl_->captures) if (c->id == capture && c->deadline == osal::Deadline::max())
        c->deadline = std::chrono::steady_clock::now() + impl_->limits.drain_timeout;
}
void OutputHub::set_name(std::uint64_t capture, const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    for (auto& c : impl_->captures) if (c->id == capture) {
        std::array<std::shared_ptr<const Source>, 2> sources;
        for (std::size_t i = 0; i < 2; ++i) {
            auto value = *c->readers[i].source; value.name = name;
            sources[i] = std::make_shared<Source>(std::move(value));
        }
        for (std::size_t i = 0; i < 2; ++i) {
            // Flush any old-label fragment before publishing under the new label.
            impl_->publish(c->readers[i], EventKind::Renamed);
            c->readers[i].source = std::move(sources[i]);
        }
        return;
    }
}
osal::Result<void> OutputHub::wait_until(osal::Deadline deadline) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->changed.wait_until(lock, deadline, [&] { return impl_->captures.empty(); }))
        return std::unexpected(error(osal::ErrorCode::Timeout));
    return {};
}
Stats OutputHub::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->counters; result.active_captures = impl_->captures.size(); return result;
}
}
