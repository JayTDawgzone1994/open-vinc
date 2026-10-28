#pragma once

// Central owner of ModuleTypes, named Modules and restart/stop policy.
// A logical Module can have several process generations; never identify a run by name or PID alone.

#include <vinc/time.hpp>
#include <vinc/control/channel.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <vinc/module/error.hpp>
#include <vinc/message_bus/message_bus.hpp>
#include <vinc/module/mid.hpp>
#include <vinc/module/module_info.hpp>
#include <vinc/module/contract_report.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace vinc {
namespace detail {
struct ManagerAccess;
}
using CommandFunction = std::function<Result<int32_t>(std::vector<std::string>&)>;
using AutoCompleteFunction = std::function<Result<std::vector<std::string>>(std::vector<std::string>&)>;

struct microProgram {
    std::string name;
    CommandFunction main_func;
    AutoCompleteFunction autoComplete_func;
};


// Explicit per-launch policy. Build configuration never selects debugging automatically.
struct ModuleStartOptions {
    bool debug = false;
};

struct ModulePolicy {
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
    std::chrono::milliseconds startup_timeout{30000};
    std::chrono::milliseconds stop_timeout{3000};
    // Total retries for this logical Module; successful starts do not reset the counter.
    std::uint32_t max_restarts = 0;
    std::chrono::milliseconds restart_delay{1000};
};

struct PythonLaunch {
    std::filesystem::path script;
    std::filesystem::path bootstrap;
    // Empty selects Python from PATH at launch. An override is an absolute executable path.
    std::filesystem::path executable;
};

// A ModuleType describes a Plugin or Python entry; registering it does not launch.
// Instance naming belongs to the Module source and its on_name hook.
struct ModuleType {
    std::string name;
    std::filesystem::path library;
    ModulePolicy policy;
    std::optional<PythonLaunch> python;
};

struct ModuleDiagnostic {
    MID mid = 0;
    Error error;
};

struct ModuleContractDiagnostic {
    std::uint64_t sequence = 0;
    MID mid = 0;
    std::uint64_t pid = 0;
    // Populated only after confirmed OS exit; neither field claims who caused it.
    bool termination_requested = false;
    // The effective fatal/startup/explicit-stop deadline elapsed while exit was
    // unconfirmed. This does not imply that an OS termination request succeeded.
    bool exit_deadline_expired = false;
    osal::ExitStatus exit{};
    Result<ModuleContractReport> report;
};

struct ModuleSnapshot {
    std::string name;
    std::string module_type;
    std::filesystem::path plugin_path;
    std::filesystem::path python_script;
    bool debug = false;
    bool debug_waiting = false;
    ModuleNotifications notifications;
    MID mid = 0;
    ModuleState state = ModuleState::Queued;
    // The module runtime uses mid as its channel/Message Bus generation; retain both fields.
    std::uint64_t generation = 0;
    std::uint64_t pid = 0;
    std::uint32_t restarts = 0;
    // Retain the last failing generation even after a successful restart.
    std::optional<ModuleDiagnostic> last_failure;
    std::optional<ModuleDiagnostic> cleanup_failure;
    std::optional<osal::ExitStatus> last_exit;
    std::optional<ModuleContractDiagnostic> last_contract_failure;
};

// Owns child processes. Destruction requests stop, then force termination on deadline.
// Requests are bounded, never automatically replayed, and timeout does not imply cancellation.
class ModuleManager {
public:
    static Result<std::unique_ptr<ModuleManager>> create(message_bus::Limits limits = {},
                                                          std::filesystem::path host_executable = {});
    ~ModuleManager();
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;
    // Subscribe before starting modules to receive initialization output.
    output_hub::OutputHub& output_hub() noexcept;
    Result<void> register_module_type(ModuleType module_type);
    // Queue startup and return its fresh MID before any name exists. Wait for Ready,
    // then inspect(mid).name for the approved name. Old MIDs never target a restart.
    Result<MID> start_module(const std::string& module_type, std::vector<std::string> args = {},
                             ModuleStartOptions options = {});
    // Wait only for the pre-load gate; timeout/cancellation do not stop the child.
    Result<void> wait_debug_ready(MID mid, vinc::Deadline deadline, std::stop_token stop = {});
    // Queue a one-shot release. Success is local acceptance, not Plugin readiness.
    Result<void> continue_debug(MID mid);
    Result<void> wait_running(const ModuleKey& key, vinc::Deadline deadline, std::stop_token stop = {});
    Result<void> stop_module(const ModuleKey& key, vinc::Deadline deadline);
    // Idempotent. Closes admission and asks every managed Module to stop.
    void begin_shutdown();
    // Timeout leaves supervision alive. Success includes confirmed exits, Message Bus
    // cleanup and bounded output drain (inspect output_hub stats for truncation).
    Result<void> wait_shutdown(vinc::Deadline deadline);
    std::vector<ModuleSnapshot> snapshots() const;
    // Cheap change detection; detailed snapshots still require normal locking.
    std::uint64_t contract_diagnostic_revision() const noexcept;
    Result<void> reserve_command_name(const std::string& name);
    Result<void> wait_started(const ModuleKey& key, std::stop_token stop = {});
    Result<void> forget_module(const ModuleKey& key);
    std::optional<ModuleSnapshot> inspect(const ModuleKey& key) const;
    // Canceling a wait does not undo a sent request; Busy persists until reply/exit.
    Result<int32_t> command(const ModuleKey& key, std::vector<std::string> args, vinc::Deadline deadline,
                            std::stop_token stop = {});
    Result<std::vector<std::string>> complete(const ModuleKey& key, std::vector<std::string> args,
                                              vinc::Deadline deadline);
    // Convenience calls preserve the same typed failure contract.
    // Micro-program callbacks execute in the central process and have no process isolation.
    Result<void> unregister_module_type(const std::string& name);
    Result<int32_t> call_module_type_interface(const std::string& name, std::vector<std::string> args);
    Result<int32_t> call_module_interface(const std::string& name, std::vector<std::string> args);
    Result<std::vector<std::string>> call_module_type_autoComplete(const std::string& name,
                                                                   std::vector<std::string> args);
    Result<std::vector<std::string>> call_module_autoComplete(const std::string& name,
                                                              std::vector<std::string> args);
    Result<void> register_micro_program(const std::string& name, CommandFunction main,
                                        AutoCompleteFunction completion = nullptr);
    Result<int32_t> call_micro_program(const std::string& name, std::vector<std::string> args);
    Result<std::vector<std::string>> call_micro_program_autoComplete(const std::string& name,
                                                                     std::vector<std::string> args);
    std::vector<std::string> get_registered_module_type_names() const;
    std::vector<std::string> get_active_module_names() const;
    std::vector<std::string> get_micro_program_names() const;
    bool has_module_type(const std::string& name) const;
    bool has_active_module(const std::string& name) const;
    bool has_micro_program(const std::string& name) const;

private:
    friend struct detail::ManagerAccess;
    struct Impl;
    explicit ModuleManager(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
}
