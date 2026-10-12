#pragma once
#include <vinc/log/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::log::detail {
inline constexpr std::array error_entries{ErrorCodeEntry{ErrorCode::InvalidArgument, "InvalidArgument"},
                                          ErrorCodeEntry{ErrorCode::TooLarge, "TooLarge"},
                                          ErrorCodeEntry{ErrorCode::Closed, "Closed"},
                                          ErrorCodeEntry{ErrorCode::Backpressure, "Backpressure"},
                                          ErrorCodeEntry{ErrorCode::ChannelFailed, "ChannelFailed"},
                                          ErrorCodeEntry{ErrorCode::Protocol, "Protocol"},
                                          ErrorCodeEntry{ErrorCode::FileFailed, "FileFailed"},
                                          ErrorCodeEntry{ErrorCode::CapacityExceeded, "CapacityExceeded"},
                                          ErrorCodeEntry{ErrorCode::ResourceExhausted, "ResourceExhausted"}};
inline constexpr std::array operation_entries{ErrorOperationEntry{ErrorOperation::Create, "Create"},
                                              ErrorOperationEntry{ErrorOperation::Write, "Write"},
                                              ErrorOperationEntry{ErrorOperation::Connect, "Connect"},
                                              ErrorOperationEntry{ErrorOperation::Receive, "Receive"},
                                              ErrorOperationEntry{ErrorOperation::Encode, "Encode"},
                                              ErrorOperationEntry{ErrorOperation::Decode, "Decode"},
                                              ErrorOperationEntry{ErrorOperation::Configure, "Configure"},
                                              ErrorOperationEntry{ErrorOperation::Rotate, "Rotate"},
                                              ErrorOperationEntry{ErrorOperation::Flush, "Flush"},
                                              ErrorOperationEntry{ErrorOperation::Command, "Command"}};
inline constexpr ErrorCatalog error_catalog{ErrorDomain::Log, "Log", error_entries, operation_entries};
static_assert(valid_catalog(error_catalog));
}
