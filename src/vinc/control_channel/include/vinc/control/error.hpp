#pragma once
#include <vinc/result.hpp>

namespace vinc::control {
enum class ErrorCode : std::int32_t {
    Timeout = 1,
    Closed = 2,
    Protocol = 3,
    Io = 4,
    Backpressure = 5,
    InvalidArgument = 6,
    HandshakeTimeout = 7,
    HeartbeatTimeout = 8,
    ResourceExhausted = 9,
    WorkerFailed = 10,
    PayloadTooLarge = 11
};
enum class ErrorOperation : std::int32_t {
    Create = 1,
    Attach = 2,
    Handshake = 3,
    Heartbeat = 4,
    Receive = 5,
    Send = 6,
    Poll = 7,
    WaitOpen = 8,
    WaitReceive = 9,
    Close = 10,
    EncodeFrame = 11,
    DecodeFrame = 12,
    EncodeArguments = 13,
    DecodeArguments = 14,
    WriteString = 15,
    StartWorker = 16
};
}

namespace vinc {
template <> struct ErrorCodeTraits<control::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::ControlChannel;
};

template <> struct ErrorOperationTraits<control::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::ControlChannel;
};
}
