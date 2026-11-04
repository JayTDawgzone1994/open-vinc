#pragma once
#include <vinc/module/host/module_manager.hpp>
#include <vinc/shell/socket_shell_options.hpp>
#include <vinc/shell/error.hpp>
#include <mutex>
#include <optional>

namespace vinc::shell {
// One owner shared by local/remote/Tcl routers. No thread or runtime listener
// mutation: only startup and explicit central commands consume these settings.
class NetworkParams {
public:
    explicit NetworkParams(ModuleManager& manager) : manager_(manager) {
    }
    Result<SocketShellOptions> configure(SocketShellOptions options,
        std::optional<std::string> ip, std::optional<std::uint16_t> port);
    Result<std::string> refresh();

private:
    ModuleManager& manager_;
    // Protects last accepted values across command workers, not a Param commit.
    // Corrections use ordinary set and deliberately do not promise CAS.
    std::mutex mutex_;
    bool ip_registered_ = false;
    bool port_registered_ = false;
    std::string accepted_ip_ = config::shell_socket_default_ip;
    std::uint16_t accepted_port_ = config::shell_socket_default_port;
    Result<std::string> refresh_locked();
};
}
