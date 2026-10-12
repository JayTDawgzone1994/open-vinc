#pragma once
#include <vinc/contract.hpp>
#include <vinc/param/error.hpp>

namespace vinc::param::detail {
// Preserve the failure class while adding an outer API operation. This helper is
// only for Param composition; foreign domains must first be translated explicitly.
inline Error operation_error(ErrorOperation operation, const Error& cause) noexcept {
    if (cause.domain != ErrorDomain::Param) {
        contract_violation("Param operation error requires a Param cause");
    }
    if (cause.is_operation(operation)) {
        return cause;
    }
    return wrap_error(static_cast<ErrorCode>(cause.code), operation, cause);
}
}
