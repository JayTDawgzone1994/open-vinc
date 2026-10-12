#pragma once
#include <vinc/module/host/module_manager.hpp>
#include <vinc/osal/process.hpp>

namespace vinc::detail {
// Reuses the existing bootstrap envelope; script replaces the native Plugin path.
Result<void> prepare_python_spawn(osal::SpawnOptions& options, const PythonLaunch& launch);
}
