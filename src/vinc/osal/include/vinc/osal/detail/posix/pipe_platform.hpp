#pragma once

// Internal POSIX Pipe readiness bridge implemented separately by epoll and kqueue.

#include <vinc/osal/error.hpp>
#include <vinc/osal/detail/pipe_native.hpp>

namespace vinc::osal::detail {
struct PipeMonitorImpl {
    int queue = -1;
    int wake = -1;
    ~PipeMonitorImpl();
};

vinc::Result<void> isolate_pipe_descriptor(int& fd, vinc::osal::ErrorOperation) noexcept;
vinc::Result<void> arm_pipe(PipeMonitorImpl&, int fd, IoKind) noexcept;
void disarm_pipe(PipeMonitorImpl&, int fd, IoKind) noexcept;
}
