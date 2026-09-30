#pragma once
#include "../native.hpp"
#include <sys/types.h>

namespace vinc::osal::detail {
// Private platform boundary. A successful Linux watch owns a pidfd; macOS
// registers NOTE_EXIT and returns a sentinel. No native types reach public API.
Result<int> watch_process(MonitorImpl&, pid_t) noexcept;
void release_watch(int) noexcept;
Result<TerminateResult> kill_process(pid_t, int) noexcept;
}
