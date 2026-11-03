#include <vinc/module/detail/plugin.hpp>
#include <vinc/common_error.hpp>
#include <iostream>

int main() {
    using namespace vinc;
    osal::detail::ResourceCoordinator owner;
    const auto coordination = owner.services();
    detail::RuntimeServices services;
    auto invalid = coordination;
    invalid.version += 1;
    auto rejected = detail::initialize_plugin(services, invalid);
    if (rejected || !rejected.error().is(osal::ErrorCode::IncompatibleCoordinator)) {
        return 1;
    }

    // Coordination bound, then fatal-table validation failed. The one-way
    // coordinator remains live for cleanup; no private fallback or retry exists.
    auto failed = detail::initialize_plugin(services, coordination);
    if (failed || !failed.error().is(common::ErrorCode::InvalidRuntimeServices)) {
        return 2;
    }
    auto selected = osal::detail::resource_coordination();
    if (!selected || selected->context != coordination.context) {
        return 3;
    }
    auto retry = detail::initialize_plugin(services, coordination);
    if (retry || !retry.error().is(osal::ErrorCode::CoordinatorAlreadyBound)) {
        return 4;
    }

    // No failed fatal binding leaked. Its original independently validated
    // lifetime contract still works, without extending RuntimeServices fields.
    services.report_contract = [](void*, const detail::ContractViolation&) noexcept {};
    if (!detail::bind_runtime_services(services)) {
        return 5;
    }
    detail::clear_runtime_services();
    if (!osal::detail::ResourceGuard::try_acquire()) {
        return 6;
    }
    std::cout << "Plugin initialization failure retains coordinator until image end\n";
    return 0;
}
