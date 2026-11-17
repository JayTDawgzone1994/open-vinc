#pragma once

#include <vinc/result.hpp>
#include <optional>
#include <string>

namespace vinc::message_bus::detail {
struct NameCleanupFailure {
    std::string resource_name;
    Error cleanup;
};

// One failed registration can leave both ring names for restart recovery.
struct RegistrationCleanupFailure {
    Error primary;
    std::optional<NameCleanupFailure> counter;
    std::optional<NameCleanupFailure> control;
    std::optional<NameCleanupFailure> payload;
};
}
