#include <vinc/module/detail/runtime_services.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/module/error.hpp>

namespace vinc::detail {
HostRuntimeServices::HostRuntimeServices(ContractReportStorage& storage) noexcept
    : services_{runtime_services_version, sizeof(RuntimeServices), &storage,
                [](void* context, const ContractViolation& violation) noexcept {
                    static_cast<ContractReportStorage*>(context)->report_and_exit(violation);
                }} {
}

HostRuntimeServices::~HostRuntimeServices() {
    if (host_bound_) {
        clear_runtime_services();
    }
}

Result<void> HostRuntimeServices::bind_host() noexcept {
    auto bound = bind_runtime_services(services_);
    if (!bound) {
        return std::unexpected(wrap_error(module_system::ErrorCode::ServiceFailed,
                                          module_system::ErrorOperation::BindRuntimeServices, bound.error()));
    }

    host_bound_ = true;
    return {};
}

const RuntimeServices& HostRuntimeServices::services() const noexcept {
    return services_;
}
}
