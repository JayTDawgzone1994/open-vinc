#pragma once

#include <vinc/log/types.hpp>
#include <vinc/param/detail/manager.hpp>
#include <mutex>

namespace vinc::detail {
struct LogSelection {
    log::FileOptions files;
    std::string correction;
};

// Framework integration owns Param access. Log receives ordinary options only.
// The mutex protects the last validated pair, not a Param transaction or CAS.
class LogSettings {
public:
    explicit LogSettings(param::detail::ParamManager& params) : params_(params) {
    }
    Result<LogSelection> read();
    Result<std::string> refresh();

private:
    param::detail::ParamManager& params_;
    std::mutex mutex_;
    log::FileOptions accepted_;
    bool registered_ = false;

    Result<void> register_settings();
    Result<LogSelection> read_locked();
};
}
