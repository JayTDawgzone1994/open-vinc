#pragma once

// Private Pipe backend contract. Native transfers borrow scheduler-owned buffers
// until completion, including when cancellation has already been requested.

#include <vinc/osal/pipe.hpp>
#include <array>
#include <string>

namespace vinc::osal::detail {
OsError native_error(Operation, int) noexcept;
struct PipeMonitorImpl;
class PipeMonitor {
public:
    static Result<std::unique_ptr<PipeMonitor>> create();
    ~PipeMonitor();
    void wake() noexcept;
    Result<void> wait(int milliseconds) noexcept; // -1: infinite
    Result<void> initialize_worker() noexcept;
    PipeMonitorImpl& implementation() noexcept { return *impl_; }
private:
    explicit PipeMonitor(std::unique_ptr<PipeMonitorImpl>);
    std::unique_ptr<PipeMonitorImpl> impl_;
};
struct NativePipeImpl;
struct NativePipePair;
class NativePipe {
public:
    // A blocking writer is private to stdio launch; never submit it to PipeService.
    static Result<NativePipePair> create_pair(bool blocking_writer = false);
    // Binding to an event monitor is lazy; pristine ends can be handed off.
    Result<void> attach(PipeMonitor&);
    std::intptr_t handle() const noexcept;
    static Result<void> validate_handle(std::intptr_t, IoKind);
    // Ownership changes only on success (allocation may throw).
    static Result<std::unique_ptr<NativePipe>> adopt_handle(std::intptr_t, IoKind);
    ~NativePipe();
    NativePipeImpl& implementation() noexcept { return *impl_; }
private:
    explicit NativePipe(std::unique_ptr<NativePipeImpl>);
    std::unique_ptr<NativePipeImpl> impl_;
};
struct NativePipePair {
    std::unique_ptr<NativePipe> reader;
    std::unique_ptr<NativePipe> writer;
};
struct NativeIoResult {
    IoStatus status;
    std::size_t transferred = 0;
    std::optional<OsError> error;
};
struct NativeTransferImpl;
class NativeTransfer {
public:
    NativeTransfer(); // allocation happens before an operation is accepted
    ~NativeTransfer();
    void start(NativePipe&, PipeMonitor&, std::span<std::byte>, IoKind) noexcept;
    std::optional<NativeIoResult> poll() noexcept;
    void cancel() noexcept;
private:
    std::unique_ptr<NativeTransferImpl> impl_;
};
} // namespace vinc::osal::detail
