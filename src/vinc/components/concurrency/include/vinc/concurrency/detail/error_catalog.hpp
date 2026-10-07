#pragma once

#include <vinc/concurrency/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::concurrency::detail {
inline constexpr auto error_entries = std::array{
    ErrorCodeEntry{ErrorCode::InvalidArgument, "Invalid mutex argument or owner thread"},
    ErrorCodeEntry{ErrorCode::NotFound, "Mutex resource not found"},
    ErrorCodeEntry{ErrorCode::PermissionDenied, "Mutex access denied"},
    ErrorCodeEntry{ErrorCode::ResourceExhausted, "Mutex resources exhausted"},
    ErrorCodeEntry{ErrorCode::Unsupported, "Mutex operation not supported"},
    ErrorCodeEntry{ErrorCode::Timeout, "Mutex wait timed out"},
    ErrorCodeEntry{ErrorCode::Cancelled, "Mutex wait cancelled"},
    ErrorCodeEntry{ErrorCode::Busy, "Mutex acquisition already active on this thread or handle"},
    ErrorCodeEntry{ErrorCode::SystemError, "Mutex system operation failed"},
    ErrorCodeEntry{ErrorCode::Closed, "Mutex is closed"},
    ErrorCodeEntry{ErrorCode::WouldBlock, "Mutex cannot be acquired immediately"},
    ErrorCodeEntry{ErrorCode::AlreadyExists, "Mutex resource already exists"}};

inline constexpr auto operation_entries = std::array{
    ErrorOperationEntry{ErrorOperation::Open, "Open"},
    ErrorOperationEntry{ErrorOperation::Lock, "Lock"},
    ErrorOperationEntry{ErrorOperation::Unlock, "Unlock"},
    ErrorOperationEntry{ErrorOperation::Create, "Create"}};

inline constexpr ErrorCatalog error_catalog{
    ErrorDomain::Concurrency, "Concurrency", error_entries, operation_entries};
static_assert(valid_catalog(error_catalog));
}
