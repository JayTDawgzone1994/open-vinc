#pragma once
#include <vinc/bag/error.hpp>
#include <vinc/error_catalog.hpp>
#include <array>

namespace vinc::bag::detail {
inline constexpr std::array error_entries{ErrorCodeEntry{ErrorCode::InvalidArgument, "InvalidArgument"},
                                          ErrorCodeEntry{ErrorCode::Closed, "Closed"},
                                          ErrorCodeEntry{ErrorCode::CapacityExceeded, "CapacityExceeded"},
                                          ErrorCodeEntry{ErrorCode::FileFailed, "FileFailed"},
                                          ErrorCodeEntry{ErrorCode::DatabaseFailed, "DatabaseFailed"},
                                          ErrorCodeEntry{ErrorCode::InvalidFormat, "InvalidFormat"},
                                          ErrorCodeEntry{ErrorCode::ResourceExhausted, "ResourceExhausted"},
                                          ErrorCodeEntry{ErrorCode::BusFailed, "BusFailed"},
                                          ErrorCodeEntry{ErrorCode::StorageFailed, "StorageFailed"},
                                          ErrorCodeEntry{ErrorCode::AlreadyExists, "AlreadyExists"},
                                          ErrorCodeEntry{ErrorCode::WorkerFailed, "WorkerFailed"},
                                          ErrorCodeEntry{ErrorCode::TimestampUnavailable, "TimestampUnavailable"}};
inline constexpr std::array operation_entries{ErrorOperationEntry{ErrorOperation::Create, "Create"},
                                              ErrorOperationEntry{ErrorOperation::Write, "Write"},
                                              ErrorOperationEntry{ErrorOperation::Finish, "Finish"},
                                              ErrorOperationEntry{ErrorOperation::Inspect, "Inspect"},
                                              ErrorOperationEntry{ErrorOperation::Discover, "Discover"},
                                              ErrorOperationEntry{ErrorOperation::Command, "Command"},
                                              ErrorOperationEntry{ErrorOperation::Enqueue, "Enqueue"}};

inline std::string native_text(const NativeError& error) {
    if (error.source != NativeErrorSource::Sqlite) {
        return {};
    }

    // SQLite primary result values are stable. Preserve the full extended code
    // in the common formatter; this static catalog does not link SQLite runtime.
    switch (error.code & 0xff) {
    case 5:
        return "SQLite database busy";
    case 7:
        return "SQLite out of memory";
    case 8:
        return "SQLite database is read-only (recovery may require write access)";
    case 10:
        return "SQLite disk I/O error";
    case 11:
        return "SQLite database corrupt";
    case 13:
        return "SQLite database or disk full";
    case 14:
        return "SQLite cannot open file";
    case 26:
        return "Not a SQLite database";
    default:
        return "SQLite result " + std::to_string(error.code);
    }
}

inline constexpr ErrorCatalog error_catalog{ErrorDomain::Bag, "Bag", error_entries, operation_entries,
                                            native_text};
static_assert(valid_catalog(error_catalog));
}
