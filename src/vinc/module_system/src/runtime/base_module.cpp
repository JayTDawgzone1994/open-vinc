#include <vinc/module/base_module.hpp>
#include <utility>

namespace vinc::detail {
ModuleObject::ModuleObject(std::string name) : module_name_(std::move(name)) {
}
ModuleObject::~ModuleObject() = default;
}
