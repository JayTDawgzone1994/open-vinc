#include <vinc/bag/detail/database.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/osal/error.hpp>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::bag {
struct Recorder::Impl {
    Options options;
    detail::Database database;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<detail::Record> queue;
    std::map<std::pair<std::string, std::uint64_t>, Topic> topics;
    bool topics_dirty = false;
    Status state;
    std::thread worker;

    void fail(Error error) {
        database.rollback();
        std::lock_guard lock(mutex);
        state.failure = error;
        state.accepting = false;
        state.finished = true;
        state.pending_bytes = 0;
        state.pending_records = 0;
        queue.clear();
    }

    Result<void> write_batch(const std::vector<detail::Record>& batch, const std::vector<Topic>& discovered,
                             Counters counters, bool complete) {
        auto started = database.begin();
        if (!started) {
            return started;
        }

        for (const auto& topic : discovered) {
            auto saved = database.topic(topic);
            if (!saved) {
                return saved;
            }
        }

        for (const auto& record : batch) {
            auto image = record.encode(*record.message, options.message_bytes);
            if (!image) {
                return std::unexpected(
                    wrap_error(ErrorCode::StorageFailed, ErrorOperation::Write, image.error()));
            }

            auto written = database.append(record, image->bytes());
            if (!written) {
                return written;
            }
        }

        counters.written += batch.size();
        return database.commit(counters, complete);
    }

    void run() noexcept {
        try {
            Counters persisted;
            for (;;) {
                std::vector<detail::Record> batch;
                std::vector<Topic> discovered;
                std::uint64_t batch_bytes = 0;
                Counters counters;
                bool complete = false;
                bool successful = false;
                {
                    std::unique_lock lock(mutex);
                    // Time bounds a quiet batch's persistence delay. It is not a disk
                    // deadline and cannot preempt a stalled filesystem operation.
                    ready.wait_for(lock, config::bag_flush_interval, [&] {
                        return !state.accepting || state.pending_bytes >= config::bag_batch_bytes ||
                               queue.size() >= config::bag_batch_records;
                    });
                    while (!queue.empty() && batch.size() < config::bag_batch_records &&
                           batch_bytes < config::bag_batch_bytes) {
                        batch_bytes += queue.front().retained_bytes;
                        batch.push_back(std::move(queue.front()));
                        queue.pop_front();
                    }

                    if (topics_dirty) {
                        for (const auto& entry : topics) {
                            discovered.push_back(entry.second);
                        }

                        topics_dirty = false;
                    }

                    counters = state.counters;
                    complete = !state.accepting && queue.empty();
                    successful = !state.failure;
                }

                if (batch.empty() && discovered.empty() && counters == persisted && !complete) {
                    continue;
                }

                auto written = write_batch(batch, discovered, counters, complete && successful);
                if (!written) {
                    fail(written.error());
                    return;
                }

                persisted = counters;
                persisted.written += batch.size();
                const auto batch_count = batch.size();
                // Keep credit until SQLite has committed and the local owners are gone.
                batch.clear();
                {
                    std::lock_guard lock(mutex);
                    state.counters.written += batch_count;
                    state.pending_bytes -= batch_bytes;
                    state.pending_records -= batch_count;
                }

                if (complete) {
                    auto closed = database.close();
                    if (!closed) {
                        fail(closed.error());
                        return;
                    }

                    std::lock_guard lock(mutex);
                    state.finished = true;
                    return;
                }
            }
        } catch (const std::bad_alloc&) {
            fail(Error{ErrorCode::ResourceExhausted, ErrorOperation::Write});
        } catch (...) {
            // An unexpected worker exception stops this recording, never the central process.
            fail(Error{ErrorCode::WorkerFailed, ErrorOperation::Write});
        }
    }
};

Recorder::Recorder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

Recorder::~Recorder() {
    finish();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

Result<std::unique_ptr<Recorder>> Recorder::create(const std::filesystem::path& path, Options options) {
    if (!options.queue_records || !options.queue_bytes || !options.message_bytes ||
        options.message_bytes > config::bag_message_bytes || options.message_bytes > options.queue_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Create});
    }

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    auto created = impl->database.create(path, options);
    if (!created) {
        return std::unexpected(created.error());
    }

    auto result = std::unique_ptr<Recorder>(new Recorder(std::move(impl)));
    result->impl_->state.accepting = true;
    try {
        result->impl_->worker = std::thread([self = result->impl_.get()] {
            self->run();
        });
    } catch (const std::system_error& error) {
        return std::unexpected(
            Error{ErrorCode::WorkerFailed, ErrorOperation::Create, osal::native_error(error.code())});
    }

    return result;
}

Result<void> Recorder::discover(Topic topic) {
    if (topic.name.empty() || topic.name.size() > config::bag_name_bytes ||
        topic.type.size() > config::bag_name_bytes || !topic.schema) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Discover});
    }

    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.accepting) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Discover});
    }

    auto key = std::make_pair(topic.name, topic.schema);
    if (impl_->topics.contains(key)) {
        return {};
    }

    if (impl_->topics.size() >= config::bag_max_topics) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Discover});
    }

    impl_->topics.emplace(std::move(key), std::move(topic));
    impl_->topics_dirty = true;
    return {};
}

Result<void> Recorder::enqueue(std::unique_ptr<BaseMessage> message, std::uint64_t retained_bytes,
                               Encoder encoder) {
    if (!message || !encoder || !retained_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Enqueue});
    }

    std::lock_guard lock(impl_->mutex);
    auto& state = impl_->state;
    if (!state.accepting) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Enqueue});
    }

    const auto found = impl_->topics.find({message->topic_name(), message->type_hash()});
    if (found == impl_->topics.end() || !found->second.supported) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Enqueue});
    }

    ++state.counters.received;
    if (retained_bytes > impl_->options.message_bytes) {
        ++state.counters.oversized;
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Enqueue});
    }

    if (retained_bytes > impl_->options.queue_bytes - state.pending_bytes ||
        state.pending_records >= impl_->options.queue_records) {
        ++state.counters.queue_dropped;
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Enqueue});
    }

    impl_->queue.push_back({std::move(message), retained_bytes, osal::monotonic_nanoseconds(), encoder});
    state.pending_bytes += retained_bytes;
    ++state.pending_records;
    impl_->ready.notify_one();
    return {};
}

void Recorder::observe_losses(std::uint64_t overwritten, std::uint64_t oversized) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.finished) {
        impl_->state.counters.bus_lost += overwritten;
        impl_->state.counters.oversized += oversized;
    }
}

void Recorder::finish(std::optional<Error> failure) {
    std::lock_guard lock(impl_->mutex);
    impl_->state.accepting = false;
    if (failure && !impl_->state.failure) {
        impl_->state.failure = *failure;
    }

    impl_->ready.notify_one();
}

Result<void> Recorder::wait() {
    finish();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    const auto snapshot = status();
    if (snapshot.failure) {
        return std::unexpected(*snapshot.failure);
    }

    return {};
}

Status Recorder::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}
}
