#pragma once

#include <vinc/osal/process.hpp>
#include <string_view>

namespace vinc::osal {
class Process;
}

namespace vinc::module_system::test {
enum class Point {
    BeforeSend,
    AfterSend,
    ResponseSaved,
    ReportFailure,
    Ready,
    ContractWriting,
    ContractCommitted,
    NotificationReserved,
    NotificationReady
};
// Defined only by fault-test executables; production has no hook or registry.
void checkpoint(Point, osal::Process* = nullptr, std::string_view resource = {});
Result<osal::TerminateResult> terminate_process(osal::Process&);
}
