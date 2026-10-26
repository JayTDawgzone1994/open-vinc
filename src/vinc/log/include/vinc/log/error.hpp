#pragma once
#include <vinc/result.hpp>

namespace vinc::log {
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    TooLarge = 2,
    Closed = 3,
    Backpressure = 4,
    ChannelFailed = 5,
    Protocol = 6,
    FileFailed = 7,
    CapacityExceeded = 8,
    ResourceExhausted = 9
};
enum class ErrorOperation : std::int32_t {
    Create = 1,
    Write = 2,
    Connect = 3,
    Receive = 4,
    Encode = 5,
    Decode = 6,
    Configure = 7,
    Rotate = 8,
    Flush = 9,
    Command = 10
};
}

namespace vinc {
template <> struct ErrorCodeTraits<log::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Log;
};

template <> struct ErrorOperationTraits<log::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Log;
};
}
