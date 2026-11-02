#pragma once

#include <vinc/osal/detail/resource_coordinator.hpp>
#include <vinc/osal/detail/operation_error.hpp>
#include <utility>

namespace vinc::osal::detail {
// Native helpers never acquire service/record/endpoint locks while holding this
// guard. Pipe handoff may already hold PipeState's mutex before acquisition.
// Declare unpublished FD owners after the guard so rollback closes first.
// Keep forwarding local too: ELF symbol interposition must not select another
// static OSAL image's coordinator before that image has been explicitly bound.
VINC_LOCAL inline Result<ResourceGuard> acquire_resources(ErrorOperation operation,
                                               Deadline deadline = Deadline::max(),
                                               std::stop_token stop = {}) {
    auto guard = ResourceGuard::acquire_until(deadline, stop);
    if (!guard) {
        return std::unexpected(operation_error(operation, guard.error()));
    }

    return std::move(*guard);
}

VINC_LOCAL inline Result<ResourceGuard> try_acquire_resources(ErrorOperation operation) {
    auto guard = ResourceGuard::try_acquire();
    if (!guard) {
        return std::unexpected(operation_error(operation, guard.error()));
    }

    return std::move(*guard);
}
}
