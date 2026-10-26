#pragma once
#include <vinc/param/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::param::detail {
inline constexpr std::array error_entries{
    ErrorCodeEntry{ErrorCode::InvalidName, "Invalid Name"},
    ErrorCodeEntry{ErrorCode::InvalidValue, "Invalid Value"},
    ErrorCodeEntry{ErrorCode::TypeMismatch, "Type Mismatch"},
    ErrorCodeEntry{ErrorCode::NotFound, "Not Found"},
    ErrorCodeEntry{ErrorCode::AlreadyRegistered, "Already Registered"},
    ErrorCodeEntry{ErrorCode::PermissionDenied, "Permission Denied"},
    ErrorCodeEntry{ErrorCode::CapacityExceeded, "Capacity Exceeded"},
    ErrorCodeEntry{ErrorCode::StorageFailed, "Storage Failed"},
    ErrorCodeEntry{ErrorCode::FileFailed, "File Failed"},
    ErrorCodeEntry{ErrorCode::InvalidFile, "Invalid File"},
    ErrorCodeEntry{ErrorCode::Protocol, "Protocol"},
    ErrorCodeEntry{ErrorCode::Closed, "Closed"},
    ErrorCodeEntry{ErrorCode::Timeout, "Timeout"},
    ErrorCodeEntry{ErrorCode::Cancelled, "Cancelled"},
    ErrorCodeEntry{ErrorCode::ChannelFailed, "Channel Failed"},
    ErrorCodeEntry{ErrorCode::ResourceExhausted, "Resource Exhausted"},
    ErrorCodeEntry{ErrorCode::Busy, "Busy"},
    ErrorCodeEntry{ErrorCode::TimeoutOutcomeUnknown, "Timed out; parameter operation outcome unknown"},
    ErrorCodeEntry{ErrorCode::CancelledOutcomeUnknown, "Wait cancelled; parameter operation outcome unknown"},
    ErrorCodeEntry{ErrorCode::CallbackFailed, "Parameter callback threw an exception"},
    ErrorCodeEntry{ErrorCode::VersionExhausted, "Version Exhausted"}};
inline constexpr std::array operation_entries{ErrorOperationEntry{ErrorOperation::Create, "Create"},
                                              ErrorOperationEntry{ErrorOperation::Register, "Register"},
                                              ErrorOperationEntry{ErrorOperation::Watch, "Watch"},
                                              ErrorOperationEntry{ErrorOperation::Set, "Set"},
                                              ErrorOperationEntry{ErrorOperation::Update, "Update"},
                                              ErrorOperationEntry{ErrorOperation::Pump, "Pump"},
                                              ErrorOperationEntry{ErrorOperation::Load, "Load"},
                                              ErrorOperationEntry{ErrorOperation::Save, "Save"},
                                              ErrorOperationEntry{ErrorOperation::Encode, "Encode"},
                                              ErrorOperationEntry{ErrorOperation::Decode, "Decode"},
                                              ErrorOperationEntry{ErrorOperation::Attach, "Attach"},
                                              ErrorOperationEntry{ErrorOperation::Receive, "Receive"},
                                              ErrorOperationEntry{ErrorOperation::Shutdown, "Shutdown"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Param, "Param", error_entries, operation_entries};
static_assert(valid_catalog(error_catalog));
}
