#include <vinc/contract.hpp>
#include <vinc/detail/runtime_services.hpp>

namespace vinc {
[[noreturn]] void contract_violation(const char* reason, std::source_location location) noexcept {
    detail::dispatch_contract_violation(
        {reason, location.file_name(), location.function_name(), location.line()});
}

[[noreturn]] void contract_index_violation(const char* reason, std::uint64_t index,
                                         std::uint64_t size, std::source_location location) noexcept {
    detail::dispatch_contract_violation(
        {reason, location.file_name(), location.function_name(), location.line(), 1, index, size});
}
}
