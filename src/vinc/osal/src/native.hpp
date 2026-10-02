#pragma once

// Private process backend boundary: native spawn, observation and wake primitives.
// The common service owns policy and records; platform files own OS resources.

#include <vinc/osal/process.hpp>

namespace vinc::osal::detail {

OsError native_error(Operation operation, int value) noexcept;
Result<void> validate_options(const SpawnOptions&);
bool valid_utf8(const std::string&) noexcept;

struct MonitorImpl;

class NativeMonitor {
public:
    static Result<std::unique_ptr<NativeMonitor>> create();
    ~NativeMonitor();
    void wake() noexcept;
    Result<void> wait(bool polling) noexcept;

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
    static Result<std::unique_ptr<NativeProcess>> spawn(const SpawnOptions&, NativeMonitor&,
                                                        const ChildPipe* = nullptr);
    ~NativeProcess();
    Result<std::optional<ExitStatus>> poll() noexcept;
    Result<TerminateResult> terminate() noexcept;
    std::uint64_t pid() const noexcept;
    bool event_driven() const noexcept;

private:
    explicit NativeProcess(std::unique_ptr<ProcessImpl>);
    std::unique_ptr<ProcessImpl> impl_;
};

} // namespace vinc::osal::detail
