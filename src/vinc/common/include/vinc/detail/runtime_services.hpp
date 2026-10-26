#pragma once

#include <vinc/detail/local.hpp>
#include <vinc/common_error.hpp>
#include <vinc/result.hpp>
#include <type_traits>

namespace vinc::detail {
inline constexpr std::uint32_t runtime_services_version = 1;
inline constexpr int contract_fallback_exit_status = 77;

// Borrowed same-process diagnostic view. No STL ownership or source_location
// representation crosses the image boundary. Text remains valid during the call.
struct ContractViolation {
    const char* reason;
    const char* file;
    const char* function;
    std::uint32_t line;
    std::uint32_t has_index = 0;
    std::uint64_t index = 0;
    std::uint64_t size = 0;
};

// Host-owned and immutable after publication. This is a same-build native ABI,
// not an IPC object or a promise of cross-compiler compatibility.
struct RuntimeServices {
    std::uint32_t version = runtime_services_version;
    std::uint32_t size = sizeof(RuntimeServices);
    void* context = nullptr;
    void (*report_contract)(void*, const ContractViolation&) noexcept = nullptr;
};
static_assert(std::is_standard_layout_v<RuntimeServices>);
static_assert(std::is_trivially_copyable_v<RuntimeServices>);
static_assert(std::is_standard_layout_v<ContractViolation>);
static_assert(std::is_trivially_copyable_v<ContractViolation>);

// One successful bind per image lifetime; failed validation consumes no binding.
// Caller provides a real, readable table and serializes lifecycle operations.
// Bind before user work. The table, context and callback code outlive all users.
VINC_LOCAL Result<void> bind_runtime_services(const RuntimeServices& services) noexcept;

// Stop/join application work first. Withdraws the local pointer, then preserves
// the borrowed Host state until in-flight fatal readers leave or the process exits.
// As with the former reporter, a nonreturning fatal callback can prevent return;
// central's fatal supervision owns that termination policy. No hot unload support.
VINC_LOCAL void clear_runtime_services() noexcept;

// Only fatal paths enter here. No allocation, ordinary framework mutex or registry.
[[noreturn]] VINC_LOCAL void dispatch_contract_violation(const ContractViolation& violation) noexcept;
}
