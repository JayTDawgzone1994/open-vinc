#pragma once
#include <vinc/result.hpp>

namespace vinc::concurrency {
// Preserve existing identifiers; retired codes (8, 10, 13, 15-21) stay reserved.
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
    AlreadyExists = 22,
    InvalidLayout = 23
};
enum class ErrorOperation : std::int32_t {
    Unspecified = 0,
    Open = 1,
    Lock = 2,
    Unlock = 3,
    // The legacy wrapper used zero for Create; common reserves zero for unspecified.
    Create = 4,
    InspectOwner = 5,
    Recover = 6,
    Publish = 7
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
