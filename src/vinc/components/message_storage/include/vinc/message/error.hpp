#pragma once

#include <vinc/result.hpp>

namespace vinc::message {
// Preserve the original numeric codes. Zero is a failure; Result owns the success state.
enum class ErrorCode : std::int32_t {
    CapacityExceeded = 0,
    ResourceExhausted = 1,
    SizeOverflow = 2,
    OutOfRange = 3,
    StaleHandle = 4,
    Closed = 5,
    InvalidData = 6,
    TypeMismatch = 7,
    InvalidArgument = 8
};

enum class ErrorOperation : std::int32_t {
    Unspecified = 0,
    CreateRecord = 1,
    AddRoot = 2,
    AccessRoot = 3,
    Inspect = 4,
    AccessChild = 5,
    Read = 6,
    Write = 7,
    Resize = 8,
    Reserve = 9,
    Append = 10,
    Assign = 11,
    Copy = 12,
    Shrink = 13,
    Compact = 14,
    Usage = 15,
    Snapshot = 16,
    ValidateImage = 17,
    Borrow = 18,
    Seal = 19,
    Publish = 20,
    CheckHandle = 21,
    ReadView = 22,
    PopBack = 23,
    Import = 24,
    Export = 25,
    Serialize = 26,
    Bind = 27,
    Deserialize = 28
};
}

namespace vinc {
template <> struct ErrorCodeTraits<message::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::MessageStorage;
};

template <> struct ErrorOperationTraits<message::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::MessageStorage;
};
}
