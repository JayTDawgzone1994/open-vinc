#pragma once

// Private process backend boundary: native spawn, observation and wake primitives.
// The common service owns policy and records; platform files own OS resources.

#include <vinc/osal/error.hpp>
#include <vinc/osal/process.hpp>
#include <array>

namespace vinc::osal::detail {

vinc::Error native_error(vinc::osal::ErrorOperation operation, int value) noexcept;
vinc::Result<void> validate_options(const SpawnOptions&);
bool valid_utf8(const std::string&) noexcept;

struct MonitorImpl;

// Consumed one-shot notifications carry service identities, never borrowed pointers.
// Match the native monitor's bounded event batch; waiting must not allocate.
struct ProcessExitEvents {
    static constexpr std::size_t capacity = 32;
    std::array<ProcessId, capacity> ids{};
    std::size_t count = 0;
};

class NativeMonitor {
public:
    static vinc::Result<std::unique_ptr<NativeMonitor>> create();
    ~NativeMonitor();
    void wake() noexcept;
    vinc::Result<ProcessExitEvents> wait(bool polling) noexcept;

    MonitorImpl& implementation() noexcept {
        return *impl_;
    }

private:
    explicit NativeMonitor(std::unique_ptr<MonitorImpl>);
    std::unique_ptr<MonitorImpl> impl_;
};

struct ProcessImpl;

class NativeProcess {
public:
    static vinc::Result<std::unique_ptr<NativeProcess>> spawn(const SpawnOptions&, NativeMonitor&, ProcessId,
                                                              const ChildPipe* = nullptr);
    ~NativeProcess();
    // ObservationLost revokes exclusive child ownership (POSIX ECHILD); the
    // service releases this owner and must never fall back to signalling its PID.
    // Other errors retain this object for retry/control through the same identity.
    vinc::Result<std::optional<ExitStatus>> poll() noexcept;
    vinc::Result<TerminateResult> terminate() noexcept;
    std::uint64_t pid() const noexcept;
    bool event_driven() const noexcept;

private:
    explicit NativeProcess(std::unique_ptr<ProcessImpl>);
    std::unique_ptr<ProcessImpl> impl_;
};

} // namespace vinc::osal::detail
