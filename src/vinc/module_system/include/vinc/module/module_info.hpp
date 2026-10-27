#pragma once
#include <vinc/module/mid.hpp>
#include <string>
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

// Public diagnostic identity only: never expose launch paths or arguments.
struct ModuleInfo {
    MID mid = 0;
    std::string name;
    std::string module_type;
    ModuleState state = ModuleState::Queued;
};

struct ModulePage {
    std::vector<ModuleInfo> modules;
    MID next = 0;
};
}
