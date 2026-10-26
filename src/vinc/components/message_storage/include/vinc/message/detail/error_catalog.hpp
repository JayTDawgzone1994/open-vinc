#pragma once

#include <vinc/message/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::message::detail {
inline constexpr auto error_entries =
    std::array{ErrorCodeEntry{ErrorCode::CapacityExceeded, "Message capacity exceeded"},
               ErrorCodeEntry{ErrorCode::ResourceExhausted, "Message storage resources exhausted"},
               ErrorCodeEntry{ErrorCode::SizeOverflow, "Message size overflow"},
               ErrorCodeEntry{ErrorCode::OutOfRange, "Message index out of range"},
               ErrorCodeEntry{ErrorCode::StaleHandle, "Message handle is stale"},
               ErrorCodeEntry{ErrorCode::Closed, "Message storage is closed"},
               ErrorCodeEntry{ErrorCode::InvalidData, "Invalid message image"},
               ErrorCodeEntry{ErrorCode::TypeMismatch, "Message type mismatch"},
               ErrorCodeEntry{ErrorCode::InvalidArgument, "Invalid message storage argument"}};
inline constexpr auto operation_entries =
    std::array{ErrorOperationEntry{ErrorOperation::CreateRecord, "CreateRecord"},
               ErrorOperationEntry{ErrorOperation::AddRoot, "AddRoot"},
               ErrorOperationEntry{ErrorOperation::AccessRoot, "AccessRoot"},
               ErrorOperationEntry{ErrorOperation::Inspect, "Inspect"},
               ErrorOperationEntry{ErrorOperation::AccessChild, "AccessChild"},
               ErrorOperationEntry{ErrorOperation::Read, "Read"},
               ErrorOperationEntry{ErrorOperation::Write, "Write"},
               ErrorOperationEntry{ErrorOperation::Resize, "Resize"},
               ErrorOperationEntry{ErrorOperation::Reserve, "Reserve"},
               ErrorOperationEntry{ErrorOperation::Append, "Append"},
               ErrorOperationEntry{ErrorOperation::Assign, "Assign"},
               ErrorOperationEntry{ErrorOperation::Copy, "Copy"},
               ErrorOperationEntry{ErrorOperation::Shrink, "Shrink"},
               ErrorOperationEntry{ErrorOperation::Compact, "Compact"},
               ErrorOperationEntry{ErrorOperation::Usage, "Usage"},
               ErrorOperationEntry{ErrorOperation::Snapshot, "Snapshot"},
               ErrorOperationEntry{ErrorOperation::ValidateImage, "ValidateImage"},
               ErrorOperationEntry{ErrorOperation::Borrow, "Borrow"},
               ErrorOperationEntry{ErrorOperation::Seal, "Seal"},
               ErrorOperationEntry{ErrorOperation::Publish, "Publish"},
               ErrorOperationEntry{ErrorOperation::CheckHandle, "CheckHandle"},
               ErrorOperationEntry{ErrorOperation::ReadView, "ReadView"},
               ErrorOperationEntry{ErrorOperation::PopBack, "PopBack"},
               ErrorOperationEntry{ErrorOperation::Import, "Import"},
               ErrorOperationEntry{ErrorOperation::Export, "Export"},
               ErrorOperationEntry{ErrorOperation::Serialize, "Serialize"},
               ErrorOperationEntry{ErrorOperation::Bind, "Bind"},
               ErrorOperationEntry{ErrorOperation::Deserialize, "Deserialize"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::MessageStorage, "MessageStorage", error_entries,
                                            operation_entries};
static_assert(valid_catalog(error_catalog));
}
