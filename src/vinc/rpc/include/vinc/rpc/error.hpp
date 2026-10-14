#pragma once
#include <vinc/result.hpp>

namespace vinc::rpc {
enum class ErrorCode : std::int32_t {
    InvalidName = 1,
    InvalidOptions = 2,
    AlreadyRegistered = 3,
    NotFound = 4,
    TypeMismatch = 5,
    QueueFull = 6,
    CapacityExceeded = 7,
    StorageFailed = 8,
    ChannelFailed = 9,
    Protocol = 10,
    Closed = 11,
    TimeoutOutcomeUnknown = 12,
    TargetExited = 13,
    CallbackFailed = 14,
    IdExhausted = 15,
    ResourceExhausted = 16
};
enum class ErrorOperation : std::int32_t {
    Register = 1,
    Call = 2,
    Send = 3,
    Receive = 4,
    Pump = 5,
    Create = 6,
    Release = 7,
    Shutdown = 8
};
}

namespace vinc {
template <> struct ErrorCodeTraits<rpc::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Rpc;
};

template <> struct ErrorOperationTraits<rpc::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Rpc;
};
}
