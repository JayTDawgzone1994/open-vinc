#include <vinc/detail/runtime_services.hpp>
#include <atomic>
#include <cstdlib>
#include <thread>

namespace vinc::detail {
namespace {
std::atomic<const RuntimeServices*> services{nullptr};
std::atomic<std::uint64_t> readers{0};
std::atomic<bool> bound_once{false};
static_assert(decltype(services)::is_always_lock_free);
static_assert(decltype(readers)::is_always_lock_free);
static_assert(decltype(bound_once)::is_always_lock_free);
}

Result<void> bind_runtime_services(const RuntimeServices& binding) noexcept {
    using common::ErrorCode;
    using common::ErrorOperation;
    if (binding.version != runtime_services_version) {
        return std::unexpected(Error{ErrorCode::IncompatibleRuntimeServices, ErrorOperation::BindRuntimeServices});
    }

    if (binding.size < sizeof(RuntimeServices) || !binding.report_contract) {
        return std::unexpected(Error{ErrorCode::InvalidRuntimeServices, ErrorOperation::BindRuntimeServices});
    }

    bool expected = false;
    if (!bound_once.compare_exchange_strong(expected, true)) {
        return std::unexpected(Error{ErrorCode::RuntimeServicesAlreadyBound, ErrorOperation::BindRuntimeServices});
    }

    services.store(&binding);
    return {};
}

void clear_runtime_services() noexcept {
    services.store(nullptr);
    // Sequential consistency pairs withdrawal with increment-before-load below:
    // either clear sees an existing reader, or a later reader sees null.
    while (readers.load() != 0) {
        std::this_thread::yield();
    }
}

[[noreturn]] void dispatch_contract_violation(const ContractViolation& violation) noexcept {
    readers.fetch_add(1);
    if (const auto* active = services.load()) {
        active->report_contract(active->context, violation);
    }

    readers.fetch_sub(1);
    // Missing/withdrawn services and an unexpectedly returning callback all end
    // here. User terminate handlers, destructors and stdio flushes cannot delay it.
    std::_Exit(contract_fallback_exit_status);
}
}
