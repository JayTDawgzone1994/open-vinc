#pragma once
#include <vinc/module/mid.hpp>
#include <string>
#include <optional>
#include <vinc/error.hpp>
#include <vector>

namespace vinc {
enum class ModuleState : std::uint32_t {
    Queued = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    RestartWaiting = 4,
    Stopped = 5,
    Failed = 6
};

struct NotificationHealth {
    // Host-observed native health, not delivery acknowledgement or callback health.
    // Counters and the last error survive recovery for this Module generation.
    bool degraded = false;
    std::uint64_t target_mid = 0;
    std::uint64_t failures = 0;
    std::uint64_t reopen_attempts = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t first_failure = 0; // Unix nanoseconds; zero when unavailable or never failed.
    std::optional<Error> last_error;
};

struct ModuleNotifications {
    bool observed = false;
    NotificationHealth receiver;
    NotificationHealth local_sender;
    NotificationHealth publisher_routes;
};

// Public diagnostic identity only: never expose launch paths or arguments.
struct ModuleInfo {
    MID mid = 0;
    std::string name;
    std::string module_type;
    ModuleState state = ModuleState::Queued;
    ModuleNotifications notifications;
};

struct ModulePage {
    std::vector<ModuleInfo> modules;
    MID next = 0;
};
}
