#pragma once
#include <vinc/ui/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>
namespace vinc::ui::detail {
inline constexpr std::array error_entries{
    ErrorCodeEntry{ErrorCode::InvalidArgument, "InvalidArgument"},
    ErrorCodeEntry{ErrorCode::FileFailed, "FileFailed"},
    ErrorCodeEntry{ErrorCode::InvalidPackage, "InvalidPackage"},
    ErrorCodeEntry{ErrorCode::CapacityExceeded, "CapacityExceeded"},
    ErrorCodeEntry{ErrorCode::BindFailed, "BindFailed"},
    ErrorCodeEntry{ErrorCode::WorkerFailed, "WorkerFailed"}
};
inline constexpr std::array operation_entries{
    ErrorOperationEntry{ErrorOperation::Load, "Load"},
    ErrorOperationEntry{ErrorOperation::Start, "Start"},
    ErrorOperationEntry{ErrorOperation::Command, "Command"}
};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Ui, "Ui", error_entries, operation_entries};
static_assert(valid_catalog(error_catalog));
}
