#pragma once
#include <vinc/result.hpp>

namespace vinc::concurrency {
// Preserve existing identifiers. Retired service/network codes (8, 10, 13,
// 15-21) stay reserved; FileLock composition does not expose those categories.
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    NotFound = 2,
    PermissionDenied = 3,
    ResourceExhausted = 4,
    Unsupported = 5,
    Timeout = 6,
    Cancelled = 7,
    Busy = 9,
    SystemError = 11,
    Closed = 12,
    WouldBlock = 14,
    AlreadyExists = 22
};
enum class ErrorOperation : std::int32_t {
    Unspecified = 0,
    Open = 1,
    Lock = 2,
    Unlock = 3,
    // The legacy wrapper used zero for Create; common reserves zero for unspecified.
    Create = 4
};
} // namespace vinc::concurrency

namespace vinc {
template <> struct ErrorCodeTraits<concurrency::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Concurrency;
};

template <> struct ErrorOperationTraits<concurrency::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Concurrency;
};
}
