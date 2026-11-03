#pragma once
#include <vinc/module/host/module_manager.hpp>
#include <vinc/module/detail/lock_descriptor.hpp>
#include <vinc/param/types.hpp>
#include <vinc/log/types.hpp>
#include <vinc/module/detail/system_module.hpp>

namespace vinc::detail {
// Framework composition/testing only. The convenience create overload uses the fixed open-vinc identity.
struct ManagerAccess {
    static Result<void> register_system_module(ModuleManager&, ModuleType, SystemModuleOptions);
    static Result<MID> start_system_module(ModuleManager&, const std::string& type,
                                           std::vector<std::string> args = {});
    // Serializes a service toggle with commands and restart admission. Success
    // while recovering stores intent; it does not assert current availability.
    static Result<void> set_system_enabled(ModuleManager&, const std::string& name, bool enabled,
                                           Deadline, std::stop_token = {});
    static Result<std::unique_ptr<ModuleManager>> create(std::string name,
        message_bus::Limits limits, std::filesystem::path host);
    static Result<void> configure_log(ModuleManager&, std::filesystem::path root, log::FileOptions = {});
    static Result<std::string> log_command(ModuleManager&, const std::vector<std::string>&);
    // Uses the existing bounded Log queues; acceptance does not promise disk persistence.
    static Result<void> log_diagnostic(ModuleManager&, std::string text);
    static std::string log_take(ModuleManager&, log::View&);
    static std::string log_notification(ModuleManager&, std::uint64_t& cursor);
    static Result<LockDescriptor> create_lock(ModuleManager& manager);
    static Result<void> configure_params(ModuleManager& manager, std::filesystem::path data_root);
    static Result<std::string> param_command(ModuleManager& manager, std::vector<std::string> args, std::stop_token stop = {});
    static Result<void> register_param(ModuleManager& manager, std::string name, param::Value defaults);
    static Result<param::Snapshot> param_snapshot(ModuleManager& manager);
    static std::string param_diagnostic(ModuleManager& manager, std::uint64_t& cursor);
};
}
