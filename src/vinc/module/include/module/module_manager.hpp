#pragma once

// Central owner of ModuleTypes, named Modules and restart/stop policy.
// A logical Module can have several process generations; never identify a run by name or PID alone.

#include <vinc/control/channel.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/output/hub.hpp>
#include <vinc_define.hpp>
#include <vinc/bus/bus.hpp>
#include <module/mid.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace vinc {
using CommandFunction = std::function<int32_t(std::vector<std::string>&)>;
using AutoCompleteFunction = std::function<std::vector<std::string>(std::vector<std::string>&)>;

struct microProgram {
    std::string name;
    CommandFunction main_func;
    AutoCompleteFunction autoComplete_func;
};
enum class ModuleState {
    Queued,
    Starting,
    Running,
    Stopping,
    RestartWaiting,
    Stopped,
    Failed
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

// A ModuleType describes an executable and naming/policy defaults; registering it does not launch.
// module_base_name is a proposal for the child's on_name hook, never auto-suffixed.
struct ModuleType {
    std::string name;
    std::filesystem::path executable;
    std::string module_base_name;
    ModulePolicy policy;
};

struct ModuleSnapshot {
    std::string name;
    std::string module_type;
    MID mid = 0;
    ModuleState state = ModuleState::Queued;
    // The module runtime uses mid as its channel/Bus generation; retain both fields.
    std::uint64_t generation = 0;
    std::uint64_t pid = 0;
    std::uint32_t restarts = 0;
    std::string last_error;
    std::optional<osal::OsError> os_error;
    std::optional<osal::ExitStatus> last_exit;
};

// Owns child processes. Destruction requests stop, then force termination on deadline.
// Requests are bounded, never automatically replayed, and timeout does not imply cancellation.
class ModuleManager {
public:
    explicit ModuleManager(bus::Limits limits = {});
    ~ModuleManager();
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;
    static ModuleManager& get();
    // Subscribe before starting modules to receive initialization output.
    output::OutputHub& output_hub() noexcept;
    control::Result<void> register_module_type(ModuleType module_type);
    // Queue startup and return its fresh MID before any name exists. Wait for Ready,
    // then inspect(mid).name for the approved name. Old MIDs never target a restart.
    control::Result<MID> start_module(const std::string& module_type, std::vector<std::string> args = {});
    control::Result<void> wait_running(const ModuleKey& key, control::Deadline deadline,
                                       std::stop_token stop = {});
    control::Result<void> stop_module(const ModuleKey& key, control::Deadline deadline);
    // Idempotent. Closes admission and asks every managed Module to stop.
    void begin_shutdown();
    // Timeout leaves supervision alive. Success includes confirmed exits, Bus
    // cleanup and bounded output drain (inspect output_hub stats for truncation).
    control::Result<void> wait_shutdown(control::Deadline deadline);
    std::vector<ModuleSnapshot> snapshots() const;
    control::Result<void> reserve_command_name(const std::string& name);
    control::Result<void> wait_started(const ModuleKey& key, std::stop_token stop = {});
    control::Result<void> forget_module(const ModuleKey& key);
    std::optional<ModuleSnapshot> inspect(const ModuleKey& key) const;
    // Canceling a wait does not undo a sent request; Busy persists until reply/exit.
    control::Result<int32_t> command(const ModuleKey& key, std::vector<std::string> args,
                                     control::Deadline deadline, std::stop_token stop = {});
    control::Result<std::vector<std::string>> complete(const ModuleKey& key, std::vector<std::string> args,
                                                       control::Deadline deadline);
    // Legacy convenience adapters below preserve older status-code APIs.
    // Micro-program callbacks execute in the central process and have no process isolation.
    int8_t unregister_module_type(const std::string& name);
    int32_t call_module_type_interface(const std::string& name, std::vector<std::string> args);
    int32_t call_module_interface(const std::string& name, std::vector<std::string> args);
    std::vector<std::string> call_module_type_autoComplete(const std::string& name,
                                                           std::vector<std::string> args);
    std::vector<std::string> call_module_autoComplete(const std::string& name, std::vector<std::string> args);
    int8_t register_micro_program(const std::string& name, CommandFunction main,
                                  AutoCompleteFunction completion = nullptr);
    int32_t call_micro_program(const std::string& name, std::vector<std::string> args);
    std::vector<std::string> call_micro_program_autoComplete(const std::string& name,
                                                             std::vector<std::string> args);
    std::vector<std::string> get_registered_module_type_names() const;
    std::vector<std::string> get_active_module_names() const;
    std::vector<std::string> get_micro_program_names() const;
    bool has_module_type(const std::string& name) const;
    bool has_active_module(const std::string& name) const;
    bool has_micro_program(const std::string& name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
