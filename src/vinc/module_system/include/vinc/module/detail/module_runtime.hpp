#pragma once

// Framework-owned executable bootstrap, called only by the supplied Module entry point.
#include <string>
#include <vector>
#include <vinc/module/detail/module_object.hpp>
#include <memory>

namespace vinc {
int run_module_process(const std::vector<std::string>& arguments);

namespace detail {
struct RuntimeServices;
using RuntimeModule = std::unique_ptr<ModuleObject, void (*)(ModuleObject*) noexcept>;
// One source owns the code that constructs/destroys application objects. The
// runtime calls clear after services stop, but before its fatal bridge expires.
class ModuleSource {
public:
    virtual ~ModuleSource() = default;
    virtual Result<void> prepare(const std::vector<std::string>& arguments,
                                 const RuntimeServices& services) = 0;
    virtual Result<RuntimeModule> create() = 0;
    virtual void clear() noexcept = 0;
    virtual bool fatal_command_errors() const noexcept {
        return false;
    }
};
int run_module_runtime(const std::vector<std::string>& arguments, ModuleSource& source);
}
}
