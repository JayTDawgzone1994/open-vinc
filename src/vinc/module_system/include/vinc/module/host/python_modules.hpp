#pragma once
#include <vinc/module/host/module_manager.hpp>

namespace vinc {
struct PythonDiscoveryIssue {
    std::filesystem::path path;
    Error error;
};
struct PythonDiscoveryReport {
    std::size_t registered = 0;
    std::vector<PythonDiscoveryIssue> issues;
};

// One directory level, no script execution. Invalid entries are isolated;
// failure to enumerate the root is returned to the caller for diagnosis.
Result<PythonDiscoveryReport> discover_python_modules(ModuleManager& manager,
    const std::filesystem::path& root, const std::filesystem::path& bootstrap,
    const std::filesystem::path& executable = {});
}
