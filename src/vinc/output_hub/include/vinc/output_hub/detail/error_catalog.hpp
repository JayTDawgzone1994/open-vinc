#pragma once
#include <vinc/error_catalog.hpp>
#include <vinc/output_hub/error.hpp>
#include <array>

namespace vinc::output_hub::detail {
inline constexpr auto error_entries =
    std::array{ErrorCodeEntry{ErrorCode::InvalidArgument, "Invalid output settings"},
               ErrorCodeEntry{ErrorCode::ResourceExhausted, "Output resources exhausted"},
               ErrorCodeEntry{ErrorCode::Timeout, "Output drain timed out"},
               ErrorCodeEntry{ErrorCode::ReadFailed, "Output capture failed"},
               ErrorCodeEntry{ErrorCode::SpawnFailed, "Captured process could not start"},
               ErrorCodeEntry{ErrorCode::ServiceFailed, "Output PipeService failed"},
               ErrorCodeEntry{ErrorCode::WorkerFailed, "Output worker could not start"}};
inline constexpr auto operation_entries = std::array{ErrorOperationEntry{ErrorOperation::Subscribe, "Subscribe"},
                                                     ErrorOperationEntry{ErrorOperation::Spawn, "Spawn"},
                                                     ErrorOperationEntry{ErrorOperation::ReadOutput, "ReadOutput"},
                                                     ErrorOperationEntry{ErrorOperation::Wait, "Wait"},
                                                     ErrorOperationEntry{ErrorOperation::Create, "Create"},
                                                     ErrorOperationEntry{ErrorOperation::StartWorker, "StartWorker"},
                                                     ErrorOperationEntry{ErrorOperation::MonitorOutput, "MonitorOutput"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::OutputHub, "OutputHub", error_entries,
                                            operation_entries};
static_assert(valid_catalog(error_catalog));
}
