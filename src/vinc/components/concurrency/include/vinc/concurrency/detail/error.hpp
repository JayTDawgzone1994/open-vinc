#pragma once

#include <vinc/concurrency/error.hpp>

namespace vinc::concurrency::detail {
// Internal boundary shared by the mutex implementation and its error tests.
// Translate only FileLock's vocabulary; preserve unknown failures as causes.
vinc::Error translate_file_lock_error(const vinc::Error& failure, ErrorOperation operation) noexcept;
}
