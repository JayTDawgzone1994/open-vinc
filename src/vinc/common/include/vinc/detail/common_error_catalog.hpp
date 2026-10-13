#pragma once

#include <vinc/common_error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::common::detail {
inline constexpr auto codes = std::array{
    ErrorCodeEntry{ErrorCode::InvalidRuntimeServices, "Invalid runtime services table"},
    ErrorCodeEntry{ErrorCode::IncompatibleRuntimeServices, "Incompatible runtime services version"},
    ErrorCodeEntry{ErrorCode::RuntimeServicesAlreadyBound, "Runtime services already bound"},
    ErrorCodeEntry{ErrorCode::TimestampOutOfRange, "System time is outside unsigned Unix nanoseconds"}};
inline constexpr auto operations = std::array{
    ErrorOperationEntry{ErrorOperation::BindRuntimeServices, "BindRuntimeServices"},
    ErrorOperationEntry{ErrorOperation::ReadTime, "ReadTime"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Common, "Common", codes, operations};
static_assert(valid_catalog(error_catalog));
}
