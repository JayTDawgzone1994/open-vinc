#pragma once

#include <vinc/concurrency/error.hpp>

namespace vinc::concurrency::detail {
// Internal boundary shared by the mutex implementation and its error tests.
// Translate shared resource failures; preserve unknown failures as causes.
vinc::Error translate_shared_memory_error(const vinc::Error& failure, ErrorOperation operation) noexcept;
}
