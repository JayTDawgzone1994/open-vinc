#include <vinc/ui/detail/graph.hpp>
#include <vinc/config.hpp>

namespace vinc::ui::detail {
Result<std::string> GraphRequests::request() {
    std::unique_lock lock(mutex_);
    if (stopped_) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::Graph});
    }

    if (job_) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Graph});
    }

    auto job = std::make_shared<Job>();
    job->deadline = std::chrono::steady_clock::now() + config::ui_graph_timeout;
    job_ = job;
    const bool finished = changed_.wait_until(lock, job->deadline, [&] {
        return stopped_ || job->result.has_value();
    });
    if (stopped_) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::Graph});
    }

    if (!finished || std::chrono::steady_clock::now() >= job->deadline) {
        job->expired = true;
        if (!job->running) {
            job_.reset();
        }
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::Graph});
    }

    return std::move(*job->result);
}

void GraphRequests::process() {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || !job_ || job_->running) {
            return;
        }
        job = job_;
        job->running = true;
    }

    Result<std::string> result;
    try {
        result = loader_(job->deadline);
    } catch (const std::bad_alloc&) {
        result = std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Graph});
    }
    {
        std::lock_guard lock(mutex_);
        if (!stopped_ && !job->expired) {
            job->result = std::move(result);
        }
        job_.reset();
        changed_.notify_all();
    }
}

void GraphRequests::stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    changed_.notify_all();
}
}
