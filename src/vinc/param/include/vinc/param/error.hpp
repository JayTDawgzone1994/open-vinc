#pragma once
#include <vinc/result.hpp>

namespace vinc::param {
enum class ErrorCode : std::int32_t {
    InvalidName = 1,
    InvalidValue = 2,
    TypeMismatch = 3,
    NotFound = 4,
    AlreadyRegistered = 5,
    PermissionDenied = 6,
    CapacityExceeded = 7,
    StorageFailed = 8,
    FileFailed = 9,
    InvalidFile = 10,
    Protocol = 11,
    Closed = 12,
    Timeout = 13,
    Cancelled = 14,
    ChannelFailed = 15,
    ResourceExhausted = 16,
    Busy = 17,
    VersionExhausted = 18,
    CallbackFailed = 19,
    TimeoutOutcomeUnknown = 20,
    CancelledOutcomeUnknown = 21
};
enum class ErrorOperation : std::int32_t {
    Create = 1,
    Register = 2,
    Watch = 3,
    Set = 4,
    Update = 5,
    Pump = 6,
    Load = 7,
    Save = 8,
    Encode = 9,
    Decode = 10,
    Attach = 11,
    Receive = 12,
    Shutdown = 13
};
}

namespace vinc {
template <> struct ErrorCodeTraits<param::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Param;
};

template <> struct ErrorOperationTraits<param::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Param;
};
}
