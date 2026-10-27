#pragma once
#include <vinc/result.hpp>

namespace vinc::message_bus {
enum class ErrorCode : std::int32_t {
    NoNewData = 1,
    NoPublishedData = 2,
    OutstandingLoan = 3,
    InvalidLoan = 4,
    Closed = 5,
    CapacityExceeded = 6,
    TypeMismatch = 7,
    ResourceExhausted = 8,
    InvalidArgument = 9,
    OperationInProgress = 10,
    Protocol = 11,
    Timeout = 12,
    Io = 13,
    InvalidData = 14,
    WorkerFailed = 15,
    TimestampUnavailable = 16
};
enum class ErrorOperation : std::int32_t {
    Create = 1,
    Submit = 2,
    Drain = 3,
    AttachRuntime = 4,
    Receive = 5,
    // Identifier 6 belonged to the retired legacy adapter; do not reuse it.
    StartWorker = 7,
    Advertise = 8,
    Subscribe = 9,
    Borrow = 10,
    Commit = 11,
    Push = 12,
    Next = 13,
    GetLast = 14,
    Refresh = 15,
    Request = 16,
    EncodeDescriptor = 17,
    CreateTopic = 18,
    CloseRegistration = 19,
    CreateControl = 20,
    MapControl = 21,
    InitializeControl = 22,
    CreatePayload = 23,
    OpenControl = 24,
    ValidateControl = 25,
    OpenPayload = 26,
    MapPayload = 27,
    CreateRingBuffer = 28,
    OpenRingBuffer = 29,
    Reserve = 30,
    Publish = 31,
    MapCounter = 32,
    InitializeCounter = 33,
    CreateCounter = 34,
    OpenCounter = 35,
    NextCount = 36,
    ListTopics = 37,
    InspectTopics = 38,
    ListTopicConnections = 39
};
}

namespace vinc {
template <> struct ErrorCodeTraits<message_bus::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::MessageBus;
};

template <> struct ErrorOperationTraits<message_bus::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::MessageBus;
};
}
