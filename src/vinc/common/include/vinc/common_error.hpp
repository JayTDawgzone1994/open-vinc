#pragma once

#include <vinc/error.hpp>

namespace vinc::common {
enum class ErrorCode : std::int32_t {
    InvalidRuntimeServices = 1,
    IncompatibleRuntimeServices = 2,
    RuntimeServicesAlreadyBound = 3,
    TimestampOutOfRange = 4
};

enum class ErrorOperation : std::int32_t {
    BindRuntimeServices = 1,
    ReadTime = 2
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
