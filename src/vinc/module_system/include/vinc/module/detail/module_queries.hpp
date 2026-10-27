#pragma once
#include <vinc/module/module_info.hpp>
#include <vinc/module/error.hpp>
#include <vinc/control/channel.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace vinc::detail {
// One bounded read-only query. The existing watchdog receives replies; timeouts
// retire the caller, and late replies are discarded without closing the session.
class ModuleQueries {
public:
    explicit ModuleQueries(std::shared_ptr<control::Session> session) : session_(std::move(session)) {}
    Result<ModulePage> list_modules(MID after, Deadline deadline);
    Result<void> receive(control::Frame frame);
    void stop() noexcept {
        stop_.request_stop();
    }
private:
    std::shared_ptr<control::Session> session_;
    std::stop_source stop_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::uint64_t next_request_ = 1;
    std::uint64_t pending_ = 0;
    std::optional<control::Bytes> response_;
};
}
