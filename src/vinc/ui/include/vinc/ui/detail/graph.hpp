#pragma once
#include <vinc/ui/error.hpp>
#include <vinc/time.hpp>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace vinc { class Context; }
namespace vinc::ui::detail {
// A single bounded mailbox: HTTP submits, Module main executes. No callback is
// invoked under this mutex, and shutdown wakes HTTP waiters before joining them.
class GraphRequests {
public:
    using Loader = std::function<Result<std::string>(Deadline)>;
    explicit GraphRequests(Loader loader) : loader_(std::move(loader)) {}
    Result<std::string> request();
    void process();
    void stop();
private:
    struct Job {
        Deadline deadline;
        std::optional<Result<std::string>> result;
        bool running = false;
        bool expired = false;
    };
    Loader loader_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<Job> job_;
    bool stopped_ = false;
};

Result<std::string> collect_graph(Context& context, Deadline deadline);
}
