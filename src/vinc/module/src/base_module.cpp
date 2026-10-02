// Local BaseModule state and access to its runtime-attached Bus client.
// Module constructors run before attachment; use bus() from lifecycle/command hooks.

#include <module/base_module.hpp>
#include <utility>

namespace vinc {
BaseModule::BaseModule(std::string name) : module_name_(std::move(name)) {
}

BaseModule::~BaseModule() = default;

bus::Client& BaseModule::bus() {
    if (!bus_) {
        throw std::logic_error("Bus is available from on_start, not the module constructor");
    }

    return *bus_;
}
}
