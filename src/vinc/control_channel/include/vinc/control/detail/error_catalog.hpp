#pragma once
#include <vinc/control/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::control::detail {
inline constexpr auto error_entries =
    std::array{ErrorCodeEntry{ErrorCode::Timeout, "Control wait timed out"},
               ErrorCodeEntry{ErrorCode::Closed, "Control session closed"},
               ErrorCodeEntry{ErrorCode::Protocol, "Invalid control protocol"},
               ErrorCodeEntry{ErrorCode::Io, "Control I/O failed"},
               ErrorCodeEntry{ErrorCode::Backpressure, "Control capacity exhausted"},
               ErrorCodeEntry{ErrorCode::InvalidArgument, "Invalid control argument"},
               ErrorCodeEntry{ErrorCode::HandshakeTimeout, "Control handshake timed out"},
               ErrorCodeEntry{ErrorCode::HeartbeatTimeout, "Peer heartbeat timed out"},
               ErrorCodeEntry{ErrorCode::ResourceExhausted, "Control resources exhausted"},
               ErrorCodeEntry{ErrorCode::WorkerFailed, "Control worker failed"},
               ErrorCodeEntry{ErrorCode::PayloadTooLarge, "Control payload limit exceeded"}};
inline constexpr auto operation_entries =
    std::array{ErrorOperationEntry{ErrorOperation::Create, "Create"},
               ErrorOperationEntry{ErrorOperation::Attach, "Attach"},
               ErrorOperationEntry{ErrorOperation::Handshake, "Handshake"},
               ErrorOperationEntry{ErrorOperation::Heartbeat, "Heartbeat"},
               ErrorOperationEntry{ErrorOperation::Receive, "Receive"},
               ErrorOperationEntry{ErrorOperation::Send, "Send"},
               ErrorOperationEntry{ErrorOperation::Poll, "Poll"},
               ErrorOperationEntry{ErrorOperation::WaitOpen, "WaitOpen"},
               ErrorOperationEntry{ErrorOperation::WaitReceive, "WaitReceive"},
               ErrorOperationEntry{ErrorOperation::Close, "Close"},
               ErrorOperationEntry{ErrorOperation::EncodeFrame, "EncodeFrame"},
               ErrorOperationEntry{ErrorOperation::DecodeFrame, "DecodeFrame"},
               ErrorOperationEntry{ErrorOperation::EncodeArguments, "EncodeArguments"},
               ErrorOperationEntry{ErrorOperation::DecodeArguments, "DecodeArguments"},
               ErrorOperationEntry{ErrorOperation::WriteString, "WriteString"},
               ErrorOperationEntry{ErrorOperation::StartWorker, "StartWorker"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::ControlChannel, "ControlChannel", error_entries,
                                            operation_entries};
static_assert(valid_catalog(error_catalog));
}
