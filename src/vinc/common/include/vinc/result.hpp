#pragma once

#include <expected>

#include <vinc/error.hpp>

namespace vinc {

// Use Result<void> for operations that only report success or failure.
template<class T>
using Result = std::expected<T, Error>;

} // namespace vinc
