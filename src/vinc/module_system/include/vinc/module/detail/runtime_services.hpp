#pragma once

#include <vinc/detail/runtime_services.hpp>

namespace vinc::detail {
class ContractReportStorage;

// Declare after storage and before Runtime workers. Stop/join Plugin work and
// clear every Plugin binding before destroying this owner; then unmap storage.
// Its address is stable because service tables are borrowed across image boundaries.
class HostRuntimeServices {
public:
    explicit HostRuntimeServices(ContractReportStorage& storage) noexcept;
    ~HostRuntimeServices();
    HostRuntimeServices(const HostRuntimeServices&) = delete;
    HostRuntimeServices& operator=(const HostRuntimeServices&) = delete;

    Result<void> bind_host() noexcept;
    const RuntimeServices& services() const noexcept;

private:
    const RuntimeServices services_;
    bool host_bound_ = false;
};
}
