#pragma once
#include <vinc/osal/detail/resource_coordinator.hpp>

// Test-only same-build ABI. Each library owns its guard; no cross-image delete.
struct CoordinatorFixture {
    vinc::Result<void> (*bind)(const vinc::osal::detail::ResourceCoordination&);
    vinc::Result<void> (*enter)(bool, vinc::Deadline, std::stop_token);
    void (*leave)();
    void* (*identity)();
    void (*observe_unload)(int*);
};
using CoordinatorQuery = const CoordinatorFixture* (*)();
