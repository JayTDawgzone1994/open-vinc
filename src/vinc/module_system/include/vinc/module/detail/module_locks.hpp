#pragma once
#include <vinc/module/detail/context_lock.hpp>
#include <vinc/module/mid.hpp>
#include <vinc/control/channel.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <condition_variable>
#include <map>
#include <mutex>

namespace vinc::detail {
// Host-only control client. The existing watchdog is its sole frame receiver.
class ModuleLocks {
public:
    ModuleLocks(std::shared_ptr<control::Session> session, MID mid);
    Result<std::unique_ptr<ContextLock>> lock(std::uint64_t id, Deadline deadline);
    Result<void> receive(control::Frame frame);
    void stop() noexcept;
private:
    struct Pending {
        std::condition_variable_any changed;
        std::optional<control::Bytes> response;
    };
    Result<std::shared_ptr<concurrency::InterprocessMutex>> lookup(std::uint64_t id, Deadline deadline);
    std::shared_ptr<control::Session> session_;
    MID mid_;
    std::stop_source stop_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<concurrency::InterprocessMutex>> locks_;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::uint64_t next_request_ = 1;
};
}
