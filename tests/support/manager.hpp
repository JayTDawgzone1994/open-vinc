#pragma once
#include <vinc/module/detail/manager_access.hpp>
#include <random>

namespace vinc::test {
inline std::string instance_name() {
    std::random_device random;
    return "test-" + std::to_string(random()) + "-" + std::to_string(random());
}

inline Result<std::unique_ptr<ModuleManager>> create_manager(message_bus::Limits limits = {},
                                                            std::filesystem::path host = {}) {
    return detail::ManagerAccess::create(instance_name(), limits, std::move(host));
}
}
