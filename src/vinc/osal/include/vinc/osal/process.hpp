#pragma once
#include <vinc/osal/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace vinc::osal {

// Service-issued identity. Never an OS PID; not reused within this host process.
struct ProcessId {
    std::uint64_t value = 0;
    friend bool operator==(ProcessId, ProcessId) = default;
};

enum class EnvironmentMode { Inherit, Replace };
struct EnvironmentEntry {
    std::string name;  // UTF-8, without '=' or NUL
    std::optional<std::string> value; // nullopt removes an inherited variable
};
enum class StdioMode { Inherit, Null };
struct SpawnOptions {
    std::filesystem::path executable; // absolute; no shell or PATH lookup
    std::vector<std::string> arguments; // UTF-8; excludes argv[0]
    std::optional<std::filesystem::path> working_directory; // absolute if set
    EnvironmentMode environment_mode = EnvironmentMode::Inherit;
    std::vector<EnvironmentEntry> environment;
    StdioMode stdin_mode = StdioMode::Null;
    StdioMode stdout_mode = StdioMode::Inherit;
    StdioMode stderr_mode = StdioMode::Inherit;
    // Isolate terminal Ctrl+C: Windows creates a group with CTRL_C disabled;
    // POSIX creates a process group with the child's PID. No new console/session.
    // terminate() still targets only this process, not its descendants.
    bool new_process_group = false;
};

enum class ExitKind { Code, Signal };
struct ExitStatus {
    ExitKind kind;
    std::uint32_t value; // full Windows exit code, POSIX exit code or signal
    bool core_dumped = false;
    friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};
enum class TerminateResult { Requested, AlreadyExited };
struct ServiceOptions {
    // Includes retained exit results. Use forget() to release completed records.
    std::size_t max_records = 256;
};

namespace detail { struct ServiceState; struct ProcessRecord; class ChildPipe; struct ProcessAccess; }

class ProcessService;
class Process {
public:
    Process() noexcept = default;
    ~Process() = default; // dropping a view does not kill or detach the child
    Process(Process&&) noexcept = default;
    Process& operator=(Process&&) noexcept = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    explicit operator bool() const noexcept;
    ProcessId id() const noexcept;
    std::uint64_t pid() const noexcept; // diagnostic only
    [[nodiscard]] Result<std::optional<ExitStatus>> try_wait();
    [[nodiscard]] Result<ExitStatus> wait_until(Deadline, std::stop_token = {});
    [[nodiscard]] Result<TerminateResult> terminate();

private:
    friend class ProcessService;
    Process(std::shared_ptr<detail::ServiceState>, std::shared_ptr<detail::ProcessRecord>);
    std::shared_ptr<detail::ServiceState> service_;
    std::shared_ptr<detail::ProcessRecord> record_;
};

class ProcessService {
public:
    [[nodiscard]] static Result<ProcessService> create(ServiceOptions = {});
    ~ProcessService(); // closes admission; existing children continue to be reaped
    ProcessService(ProcessService&&) noexcept;
    ProcessService& operator=(ProcessService&&) noexcept;
    ProcessService(const ProcessService&) = delete;
    ProcessService& operator=(const ProcessService&) = delete;

    [[nodiscard]] Result<Process> spawn(const SpawnOptions&);
    [[nodiscard]] Result<Process> find(ProcessId) const;
    [[nodiscard]] Result<TerminateResult> terminate(ProcessId);
    [[nodiscard]] Result<void> forget(ProcessId); // only after observation completes
    [[nodiscard]] Result<void> shutdown_until(Deadline, std::stop_token = {});
    // Rare native event-monitor failure: reaping continues with bounded polling.
    std::optional<OsError> monitor_error() const;

private:
    friend struct detail::ProcessAccess;
    Result<Process> spawn_impl(const SpawnOptions&, const detail::ChildPipe*);
    explicit ProcessService(std::shared_ptr<detail::ServiceState>);
    void close_admission() noexcept;
    std::shared_ptr<detail::ServiceState> state_;
};

} // namespace vinc::osal
