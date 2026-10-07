#pragma once

#include <source_location>
#include <cstdint>
#include <vinc/detail/local.hpp>

namespace vinc {
// Reports programmer misuse and never resumes execution. Diagnostics use no
// owned strings or error catalog; the handler does not throw a framework error.
[[noreturn]] VINC_LOCAL void
contract_violation(const char* reason,
                   std::source_location location = std::source_location::current()) noexcept;

// Same fatal contract, with the already-known bounds captured without formatting.
[[noreturn]] VINC_LOCAL void
contract_index_violation(const char* reason, std::uint64_t index, std::uint64_t size,
                         std::source_location location = std::source_location::current()) noexcept;
}
