#pragma once
#include <vinc/message_bus/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::message_bus::detail {
inline constexpr auto error_entries =
    std::array{ErrorCodeEntry{ErrorCode::NoNewData, "No new message"},
               ErrorCodeEntry{ErrorCode::NoPublishedData, "No published message"},
               ErrorCodeEntry{ErrorCode::OutstandingLoan, "Publisher already has a loan"},
               ErrorCodeEntry{ErrorCode::InvalidLoan, "Invalid message loan"},
               ErrorCodeEntry{ErrorCode::Closed, "Message Bus closed"},
               ErrorCodeEntry{ErrorCode::CapacityExceeded, "Message Bus capacity exceeded"},
               ErrorCodeEntry{ErrorCode::TypeMismatch, "Message type mismatch"},
               ErrorCodeEntry{ErrorCode::ResourceExhausted, "Message Bus resources exhausted"},
               ErrorCodeEntry{ErrorCode::InvalidArgument, "Invalid Message Bus argument"},
               ErrorCodeEntry{ErrorCode::OperationInProgress, "Message Bus operation in progress"},
               ErrorCodeEntry{ErrorCode::Protocol, "Invalid Message Bus protocol"},
               ErrorCodeEntry{ErrorCode::Timeout, "Message Bus wait timed out"},
               ErrorCodeEntry{ErrorCode::Io, "Message Bus I/O failed"},
               ErrorCodeEntry{ErrorCode::InvalidData, "Invalid message data"},
               ErrorCodeEntry{ErrorCode::WorkerFailed, "Message Bus worker failed"},
               ErrorCodeEntry{ErrorCode::TimestampUnavailable, "Publication timestamp unavailable"}};
inline constexpr auto operation_entries =
    std::array{ErrorOperationEntry{ErrorOperation::Create, "Create"},
               ErrorOperationEntry{ErrorOperation::Submit, "Submit"},
               ErrorOperationEntry{ErrorOperation::Drain, "Drain"},
               ErrorOperationEntry{ErrorOperation::AttachRuntime, "AttachRuntime"},
               ErrorOperationEntry{ErrorOperation::Receive, "Receive"},
               ErrorOperationEntry{ErrorOperation::StartWorker, "StartWorker"},
               ErrorOperationEntry{ErrorOperation::Advertise, "Advertise"},
               ErrorOperationEntry{ErrorOperation::Subscribe, "Subscribe"},
               ErrorOperationEntry{ErrorOperation::Borrow, "Borrow"},
               ErrorOperationEntry{ErrorOperation::Commit, "Commit"},
               ErrorOperationEntry{ErrorOperation::Push, "Push"},
               ErrorOperationEntry{ErrorOperation::Next, "Next"},
               ErrorOperationEntry{ErrorOperation::GetLast, "GetLast"},
               ErrorOperationEntry{ErrorOperation::Refresh, "Refresh"},
               ErrorOperationEntry{ErrorOperation::Request, "Request"},
               ErrorOperationEntry{ErrorOperation::EncodeDescriptor, "EncodeDescriptor"},
               ErrorOperationEntry{ErrorOperation::CreateTopic, "CreateTopic"},
               ErrorOperationEntry{ErrorOperation::CloseRegistration, "CloseRegistration"},
               ErrorOperationEntry{ErrorOperation::CreateControl, "CreateControl"},
               ErrorOperationEntry{ErrorOperation::MapControl, "MapControl"},
               ErrorOperationEntry{ErrorOperation::InitializeControl, "InitializeControl"},
               ErrorOperationEntry{ErrorOperation::CreatePayload, "CreatePayload"},
               ErrorOperationEntry{ErrorOperation::OpenControl, "OpenControl"},
               ErrorOperationEntry{ErrorOperation::ValidateControl, "ValidateControl"},
               ErrorOperationEntry{ErrorOperation::OpenPayload, "OpenPayload"},
               ErrorOperationEntry{ErrorOperation::MapPayload, "MapPayload"},
               ErrorOperationEntry{ErrorOperation::CreateRingBuffer, "CreateRingBuffer"},
               ErrorOperationEntry{ErrorOperation::OpenRingBuffer, "OpenRingBuffer"},
               ErrorOperationEntry{ErrorOperation::Reserve, "Reserve"},
               ErrorOperationEntry{ErrorOperation::Publish, "Publish"},
               ErrorOperationEntry{ErrorOperation::MapCounter, "MapCounter"},
               ErrorOperationEntry{ErrorOperation::InitializeCounter, "InitializeCounter"},
               ErrorOperationEntry{ErrorOperation::CreateCounter, "CreateCounter"},
               ErrorOperationEntry{ErrorOperation::OpenCounter, "OpenCounter"},
               ErrorOperationEntry{ErrorOperation::ListTopics, "ListTopics"},
               ErrorOperationEntry{ErrorOperation::InspectTopics, "InspectTopics"},
               ErrorOperationEntry{ErrorOperation::ListTopicConnections, "ListTopicConnections"},
               ErrorOperationEntry{ErrorOperation::NextCount, "NextCount"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::MessageBus, "MessageBus", error_entries,
                                            operation_entries};
static_assert(valid_catalog(error_catalog));
}
