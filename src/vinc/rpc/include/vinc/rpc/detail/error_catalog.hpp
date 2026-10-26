#pragma once
#include <vinc/rpc/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::rpc::detail {
inline constexpr std::array error_entries{
    ErrorCodeEntry{ErrorCode::InvalidName, "InvalidName"},
    ErrorCodeEntry{ErrorCode::InvalidOptions, "InvalidOptions"},
    ErrorCodeEntry{ErrorCode::AlreadyRegistered, "AlreadyRegistered"},
    ErrorCodeEntry{ErrorCode::NotFound, "NotFound"},
    ErrorCodeEntry{ErrorCode::TypeMismatch, "TypeMismatch"},
    ErrorCodeEntry{ErrorCode::QueueFull, "RPC queue is full; request not accepted; retry later"},
    ErrorCodeEntry{ErrorCode::CapacityExceeded, "CapacityExceeded"},
    ErrorCodeEntry{ErrorCode::StorageFailed, "StorageFailed"},
    ErrorCodeEntry{ErrorCode::ChannelFailed, "ChannelFailed"},
    ErrorCodeEntry{ErrorCode::Protocol, "Protocol"},
    ErrorCodeEntry{ErrorCode::Closed, "Closed"},
    ErrorCodeEntry{ErrorCode::TimeoutOutcomeUnknown, "TimeoutOutcomeUnknown"},
    ErrorCodeEntry{ErrorCode::TargetExited, "TargetExited"},
    ErrorCodeEntry{ErrorCode::CallbackFailed, "CallbackFailed"},
    ErrorCodeEntry{ErrorCode::IdExhausted, "IdExhausted"},
    ErrorCodeEntry{ErrorCode::ResourceExhausted, "ResourceExhausted"}};
inline constexpr std::array operation_entries{ErrorOperationEntry{ErrorOperation::Register, "Register"},
                                              ErrorOperationEntry{ErrorOperation::Call, "Call"},
                                              ErrorOperationEntry{ErrorOperation::Send, "Send"},
                                              ErrorOperationEntry{ErrorOperation::Receive, "Receive"},
                                              ErrorOperationEntry{ErrorOperation::Pump, "Pump"},
                                              ErrorOperationEntry{ErrorOperation::Create, "Create"},
                                              ErrorOperationEntry{ErrorOperation::Release, "Release"},
                                              ErrorOperationEntry{ErrorOperation::Shutdown, "Shutdown"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Rpc, "Rpc", error_entries, operation_entries};
static_assert(valid_catalog(error_catalog));
}
