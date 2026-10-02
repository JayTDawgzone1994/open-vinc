#pragma once

// Internal POSIX Pipe readiness bridge implemented separately by epoll and kqueue.

#include "../pipe_native.hpp"

namespace vinc::osal::detail {
struct PipeMonitorImpl {
    int queue = -1;
    int wake = -1;
    ~PipeMonitorImpl();
};

Result<void> isolate_pipe_descriptor(int& fd, Operation) noexcept;
Result<void> arm_pipe(PipeMonitorImpl&, int fd, IoKind) noexcept;
void disarm_pipe(PipeMonitorImpl&, int fd, IoKind) noexcept;
}
