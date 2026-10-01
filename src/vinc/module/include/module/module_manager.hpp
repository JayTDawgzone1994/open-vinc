#pragma once

// Central owner of module definitions, named instances and restart/stop policy.
// A logical instance can have several process generations; never identify a run by name or PID alone.

#include <vinc/control/channel.hpp>
#include <vinc/osal/process.hpp>
#include <vinc_define.hpp>
#include <vinc/bus/bus.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace vinc {
using CommandFunction = std::function<int32_t(std::vector<std::string>&)>;
using AutoCompleteFunction = std::function<std::vector<std::string>(std::vector<std::string>&)>;
struct microProgram { std::string name; CommandFunction main_func; AutoCompleteFunction autoComplete_func; };
enum class ModuleState { Queued, Starting, Running, Stopping, RestartWaiting, Stopped, Failed };
struct ModulePolicy {
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
    std::chrono::milliseconds startup_timeout{30000};
    std::chrono::milliseconds stop_timeout{3000};
    // Total retries for this logical instance; successful starts do not reset the counter.
    std::uint32_t max_restarts = 0;
    std::chrono::milliseconds restart_delay{1000};
};
// A factory describes an executable and naming/policy defaults; registering it does not launch.
// Each successful start allocates a final instance name, adding a suffix when needed.
struct ModuleDefinition {
    std::string name;
    std::filesystem::path executable;
    std::string node_base_name;
    ModulePolicy policy;
};
struct ModuleSnapshot {
    std::string name;
    ModuleState state = ModuleState::Queued;
    std::uint64_t generation = 0, pid = 0;
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
    control::Result<void> register_module(ModuleDefinition);
    // Queue startup and return the allocated name. Use wait_started/wait_running
    // when later work requires on_start to have completed successfully.
    control::Result<std::string> start_module(const std::string& factory, std::vector<std::string> args = {});
    control::Result<void> wait_running(const std::string&, control::Deadline, std::stop_token = {});
    control::Result<void> stop_module(const std::string&, control::Deadline);
    // Idempotent. Closes admission and asks every managed instance to stop.
    void begin_shutdown();
    // Timeout leaves supervision alive. Success includes confirmed exits and Bus cleanup.
    control::Result<void> wait_shutdown(control::Deadline);
    std::vector<ModuleSnapshot> snapshots() const;
    control::Result<void> reserve_command_name(const std::string&);
    control::Result<void> wait_started(const std::string&, std::stop_token = {});
    control::Result<void> forget_module(const std::string&);
    std::optional<ModuleSnapshot> inspect(const std::string&) const;
    // Canceling a wait does not undo a sent request; Busy persists until reply/exit.
    control::Result<int32_t> command(const std::string&, std::vector<std::string>, control::Deadline, std::stop_token = {});
    control::Result<std::vector<std::string>> complete(const std::string&, std::vector<std::string>, control::Deadline);
    // Legacy convenience adapters below preserve older status-code APIs.
    // Micro-program callbacks execute in the central process and have no process isolation.
    int8_t unregister_module_factory(const std::string&);
    int32_t call_module_interface(const std::string&, std::vector<std::string>);
    int32_t call_node_interface(const std::string&, std::vector<std::string>);
    std::vector<std::string> call_module_autoComplete(const std::string&, std::vector<std::string>);
    std::vector<std::string> call_node_autoComplete(const std::string&, std::vector<std::string>);
    int8_t register_micro_program(const std::string&, CommandFunction, AutoCompleteFunction = nullptr);
    int32_t call_micro_program(const std::string&, std::vector<std::string>);
    std::vector<std::string> call_micro_program_autoComplete(const std::string&, std::vector<std::string>);
    std::vector<std::string> get_registered_factory_names() const;
    std::vector<std::string> get_active_node_names() const;
    std::vector<std::string> get_micro_program_names() const;
    bool has_factory(const std::string&) const;
    bool has_active_node(const std::string&) const;
    bool has_micro_program(const std::string&) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
