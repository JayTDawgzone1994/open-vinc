#pragma once

// Application extension point inside a child process. The mode is an explicit
// compile-time choice; the Host retains heartbeat and shutdown supervision.
#include <vinc/module/detail/module_object.hpp>

namespace vinc {
enum class RunMode {
    FrameworkLoop,
    Manual
};

template <RunMode Mode>
class BaseModule;

template <>
class BaseModule<RunMode::FrameworkLoop> : public detail::ModuleObject {
public:
    using ModuleObject::ModuleObject;
    Result<void> main() final {
        return context.pumpLoop();
    }
};

template <>
class BaseModule<RunMode::Manual> : public detail::ModuleObject {
public:
    using ModuleObject::ModuleObject;
    Result<void> main() override = 0;
};
}

#include <vinc/module/detail/plugin.hpp>
