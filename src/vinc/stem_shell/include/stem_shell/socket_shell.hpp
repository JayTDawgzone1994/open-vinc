#pragma once
#include <stem_shell/script_service.hpp>
#include <vinc/osal/socket_address.hpp>

namespace vinc::shell {
struct SocketShellOptions {
    std::string ip = "127.0.0.1";
    std::uint16_t port = 5500;
    std::chrono::milliseconds disconnect_timeout{30000};
    std::chrono::milliseconds send_timeout{30000};
    std::size_t max_connections = 8;
};
// Plain UTF-8 TCP server; no authentication, encryption, remote editing or client mode.
// Owns its network thread and bounded sessions. The manager/service must outlive it.
class SocketShell {
public:
    SocketShell(ModuleManager&, ScriptService&, SocketShellOptions = {});
    ~SocketShell();
    SocketShell(const SocketShell&) = delete;
    SocketShell& operator=(const SocketShell&) = delete;
    osal::IpEndpoint endpoint() const;
    bool shutdown_requested() const noexcept;
    std::string failure() const;
    void begin_shutdown(); // Stop accepting commands; keep delivering final output.
    void finish_shutdown(); // Join commands and drain replies with a bounded send deadline.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
