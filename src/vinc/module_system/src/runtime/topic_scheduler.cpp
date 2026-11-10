#include <vinc/module/detail/topic_scheduler.hpp>
#include <vinc/config.hpp>
#include <vinc/osal/error.hpp>
#include <algorithm>
#include <system_error>

namespace vinc::detail {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
}

TopicScheduler::TopicScheduler(std::function<void()> wake) : wake_(std::move(wake)) {
}

TopicScheduler::~TopicScheduler() {
    stop();
}

Result<void> TopicScheduler::start(std::uint32_t workers) {
    if (workers == 0 || workers > config::module_topic_worker_limit) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::StartWorker});
    }
    if (committed_ || stopping_ || !workers_.empty()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::StartWorker});
    }

    try {
        jobs_.reserve(config::module_callback_limit);
        workers_.reserve(workers);
        for (std::uint32_t index = 0; index < workers; ++index) {
#ifdef VINC_MODULE_TESTING
            topic_scheduler_test_before_start(index);
#endif
            workers_.emplace_back([this] { run(); });
        }
    } catch (const std::bad_alloc&) {
        stop();
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::StartWorker});
    } catch (const std::system_error& error) {
        stop();
        return std::unexpected(Error{ErrorCode::ThreadFailed, ErrorOperation::StartWorker,
                                     osal::native_error(error.code())});
    }

    // No worker may dispatch before every thread was successfully created.
    {
        std::lock_guard lock(mutex_);
        committed_ = true;
    }
    changed_.notify_all();
    return {};
}

Result<void> TopicScheduler::submit(std::string topic, std::shared_ptr<Task> task) {
    // task also receives a replaced capture; destroy it only after unlocking.
    {
        std::lock_guard lock(mutex_);
        if (failure_) {
            return std::unexpected(*failure_);
        }
        if (stopping_ || !committed_) {
            return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::Pump});
        }
        auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto& job) {
            return job->topic == topic;
        });
        if (found != jobs_.end()) {
            auto& job = **found;
            if (job.active || job.ready) {
                job.next.swap(task);
            } else {
                job.current.swap(task);
                job.ready = true;
            }
        } else {
            if (jobs_.size() >= config::module_callback_limit) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Pump});
            }
            auto job = std::make_shared<Job>();
            job->topic = std::move(topic);
            job->current = std::move(task);
            job->ready = true;
            jobs_.push_back(std::move(job));
        }
    }
    changed_.notify_one();
    return {};
}

void TopicScheduler::prune() {
    std::vector<std::shared_ptr<Job>> retired;
    {
        std::lock_guard lock(mutex_);
        retired.reserve(jobs_.size());
        for (auto& job : jobs_) {
            if (!job->active && !job->ready) {
                retired.push_back(std::move(job));
            }
        }
        std::erase(jobs_, nullptr);
        if (cursor_ >= jobs_.size()) {
            cursor_ = 0;
        }
    }
}

bool TopicScheduler::ready() const {
    return committed_ && std::any_of(jobs_.begin(), jobs_.end(), [](const auto& job) {
        return job->ready && !job->active;
    });
}

std::shared_ptr<TopicScheduler::Job> TopicScheduler::claim() {
    // Round-robin admission bounds a hot Topic to one task turn per traversal.
    for (std::size_t count = 0; count < jobs_.size(); ++count) {
        if (cursor_ >= jobs_.size()) {
            cursor_ = 0;
        }
        const auto index = cursor_;
        ++cursor_;
        auto& job = jobs_[index];
        if (job->ready && !job->active) {
            job->ready = false;
            job->active = true;
            return job;
        }
    }
    return {};
}

void TopicScheduler::run() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || ready(); });
            if (stopping_) {
                return;
            }
            job = claim();
        }

        Result<bool> result;
        try {
            result = (*job->current)();
        } catch (const std::bad_alloc&) {
            result = std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Pump});
        } catch (...) {
            result = std::unexpected(Error{ErrorCode::HookException, ErrorOperation::SubscriptionCallback});
        }
        finish(job, std::move(result));
    }
}

void TopicScheduler::finish(const std::shared_ptr<Job>& job, Result<bool> result) {
    std::shared_ptr<Task> retired;
    bool failed = false;
    {
        std::lock_guard lock(mutex_);
        if (!result) {
            if (!failure_) {
                failure_ = result.error();
            }
            stopping_ = true;
            failed = true;
        } else if (!*result) {
            retired = std::move(job->current);
            job->current = std::move(job->next);
        }
        job->active = false;
        job->ready = !stopping_ && static_cast<bool>(job->current);
    }
    changed_.notify_all();
    if (failed) {
        // The native notification is advisory; fallback scanning also observes
        // failure_. A reporting exception must not escape this worker entry.
        try {
            wake_();
        } catch (...) {
        }
    }
}

void TopicScheduler::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    // All callbacks have returned. No internal mutex is held during capture destruction.
    jobs_.clear();
}

std::optional<Error> TopicScheduler::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::vector<std::thread::id> TopicScheduler::thread_ids() const {
    std::vector<std::thread::id> result;
    result.reserve(workers_.size());
    for (const auto& worker : workers_) {
        result.push_back(worker.get_id());
    }
    return result;
}
}
