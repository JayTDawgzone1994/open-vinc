#pragma once
#include <vinc/osal/error.hpp>
#include <optional>

namespace vinc::sync {
// Synchronization policies have their own operation vocabulary. OSAL errors
// remain intact as a nested cause; OSAL need not know InterprocessMutex exists.
using ErrorCode = osal::ErrorCode;
enum class Operation { Create, Open, Lock, Unlock };
struct Error {
    ErrorCode code;
    Operation operation;
    std::optional<osal::OsError> os_error;
};
template<class T> using Result = std::expected<T, Error>;
} // namespace vinc::sync
