#pragma once

#include <vinc/error.hpp>

namespace vinc::common {
enum class ErrorCode : std::int32_t {
    InvalidRuntimeServices = 1,
    IncompatibleRuntimeServices = 2,
    RuntimeServicesAlreadyBound = 3
};

enum class ErrorOperation : std::int32_t {
    BindRuntimeServices = 1
};
}

namespace vinc {
template <> struct ErrorCodeTraits<common::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Common;
};

template <> struct ErrorOperationTraits<common::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Common;
};
}
