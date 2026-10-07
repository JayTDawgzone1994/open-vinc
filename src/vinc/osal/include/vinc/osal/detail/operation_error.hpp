#pragma once

#include <vinc/contract.hpp>
#include <vinc/osal/error.hpp>

namespace vinc::osal::detail {
// Only OSAL-to-OSAL composition belongs here. Other components must select
// their own public error code instead of reinterpreting another domain's code.
inline vinc::Error operation_error(ErrorOperation operation, const vinc::Error& cause) noexcept {
    if (cause.domain != vinc::ErrorDomain::Osal) {
        vinc::contract_violation("OSAL operation error requires an OSAL cause");
    }

    if (cause.is_operation(operation)) {
        return cause;
    }

    // Stable numeric codes (including unknown values) remain unchanged. A new
    // layer adds caller context without losing the original operation or native error.
    return vinc::wrap_error(static_cast<ErrorCode>(cause.code), operation, cause);
}
} // namespace vinc::osal::detail
