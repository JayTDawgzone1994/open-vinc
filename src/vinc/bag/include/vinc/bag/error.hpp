#pragma once
#include <vinc/result.hpp>

namespace vinc::bag {
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    Closed = 2,
    CapacityExceeded = 3,
    FileFailed = 4,
    DatabaseFailed = 5,
    InvalidFormat = 6,
    ResourceExhausted = 7,
    BusFailed = 8,
    StorageFailed = 9,
    AlreadyExists = 10,
    WorkerFailed = 11,
    TimestampUnavailable = 12
};
enum class ErrorOperation : std::int32_t {
    Create = 1,
    Write = 2,
    Finish = 3,
    Inspect = 4,
    Discover = 5,
    Command = 6,
    Enqueue = 7
};
}

namespace vinc {
template <> struct ErrorCodeTraits<bag::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Bag;
};

template <> struct ErrorOperationTraits<bag::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Bag;
};
}
