#pragma once

#include <vinc/detail/runtime_services.hpp>
#include <vinc/module/contract_report.hpp>
#include <vinc/module/mid.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <memory>
#include <optional>
#include <string>

namespace vinc::detail {
inline constexpr int contract_exit_status = 76;

enum class ContractReportState { Empty, Writing, Committed };

// One stable owner per generation. Central observes only the atomic state while
// the child lives; payload is read after exit. Child mappings never unlink the name.
class ContractReportStorage {
public:
    ~ContractReportStorage();
    static Result<std::unique_ptr<ContractReportStorage>> create(std::string name, MID mid);
    static Result<std::unique_ptr<ContractReportStorage>> open(std::string name, MID mid);
    Result<std::optional<ModuleContractReport>> read_after_exit() const noexcept;
    Result<ContractReportState> observe_state() const noexcept;
    Result<void> close() noexcept;
    const std::string& name() const noexcept;
    [[noreturn]] void report_and_exit(const ContractViolation& violation) noexcept;

private:
    ContractReportStorage(std::string name, MID mid);
    Result<void> map();
    std::string name_;
    MID mid_;
    bool owns_name_ = false;
    osal::SharedMemory memory_;
    osal::SharedMemoryMapping mapping_;
    std::span<std::byte> bytes_;
};

}
