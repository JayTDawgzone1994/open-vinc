#pragma once

#include <vinc/config.hpp>
#include <vinc/time.hpp>
#include <array>
#include <string>
#include <optional>

namespace vinc::detail {
// Trusted central composition only. A Plugin never supplies these options.
struct SystemModuleOptions {
    std::string name;
    // Optional service toggle, retained centrally across process generations.
    // The owned Module accepts --enabled and enable/disable command hooks.
    std::optional<bool> enabled;
    config::SystemModuleRestartMode restart_mode = config::system_module_restart_mode;
    std::chrono::milliseconds restart_delay = config::system_module_restart_delay;
    std::chrono::milliseconds start_limit_interval = config::system_module_start_limit_interval;
    std::size_t start_limit_burst = config::system_module_start_limit_burst;
};

// Caller serializes access and validates options before use. The fixed circular
// history records attempts, not Ready events; short successful runs cannot reset it.
class SystemStartWindow {
    std::array<Deadline, config::system_module_start_limit_burst> attempts_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;

public:
    Deadline next_start(Deadline now, const SystemModuleOptions& options) const {
        if (count_ < options.start_limit_burst) {
            return now;
        }
        const auto eligible = attempts_[next_] + options.start_limit_interval;
        return eligible > now ? eligible : now;
    }

    void record_start(Deadline now, const SystemModuleOptions& options) {
        attempts_[next_] = now;
        next_ = (next_ + 1) % options.start_limit_burst;
        if (count_ < options.start_limit_burst) {
            ++count_;
        }
    }
};
}
