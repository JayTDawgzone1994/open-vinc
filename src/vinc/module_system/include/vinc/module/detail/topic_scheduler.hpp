#pragma once
#include <vinc/module/error.hpp>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vinc::detail {
// Module-local execution only. Tasks own finite batch metadata, never a backlog
// of copied payloads. true yields the same unfinished batch; false completes it.
class TopicScheduler {
public:
    using Task = std::function<Result<bool>()>;
    explicit TopicScheduler(std::function<void()> wake);
    ~TopicScheduler();
    TopicScheduler(const TopicScheduler&) = delete;
    TopicScheduler& operator=(const TopicScheduler&) = delete;
    Result<void> start(std::uint32_t workers);
    Result<void> submit(std::string topic, std::shared_ptr<Task> task);
    void prune();
    void stop();
    std::optional<Error> failure() const;
    std::vector<std::thread::id> thread_ids() const;

private:
    struct Job {
        std::string topic;
        std::shared_ptr<Task> current;
        std::shared_ptr<Task> next;
        bool active = false;
        bool ready = false;
    };
    std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> workers_;
    std::size_t cursor_ = 0;
    bool committed_ = false;
    bool stopping_ = false;
    std::optional<Error> failure_;
    bool ready() const;
    std::shared_ptr<Job> claim();
    void run();
    void finish(const std::shared_ptr<Job>& job, Result<bool> result);
};
}
