#pragma once

#include <vinc/common_error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::common::detail {
inline constexpr auto codes = std::array{
    ErrorCodeEntry{ErrorCode::InvalidRuntimeServices, "Invalid runtime services table"},
    ErrorCodeEntry{ErrorCode::IncompatibleRuntimeServices, "Incompatible runtime services version"},
    ErrorCodeEntry{ErrorCode::RuntimeServicesAlreadyBound, "Runtime services already bound"}};
inline constexpr auto operations = std::array{
    ErrorOperationEntry{ErrorOperation::BindRuntimeServices, "BindRuntimeServices"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Common, "Common", codes, operations};
static_assert(valid_catalog(error_catalog));
}
