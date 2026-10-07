#pragma once
#include <vinc/shell/socket_shell_options.hpp>
#include <vinc/shell/detail/script_service.hpp>
#include <vinc/osal/socket_address.hpp>

namespace vinc::shell {
struct SocketShellStatus {
    bool running = false;
    bool accepting = false;
    std::uint64_t failures = 0;
    std::optional<Error> last_failure;
};

// Plain UTF-8 TCP server; no authentication, encryption, remote editing or client mode.
// Internal service: central startup activates it through the framework-owned Shell.
// Owns its network thread and bounded sessions. The manager/service must outlive it.
class SocketShell {
public:
    static Result<std::unique_ptr<SocketShell>> create(ModuleManager& manager, ScriptService& scripts,
                                                       SocketShellOptions options = {});
    ~SocketShell();
    SocketShell(const SocketShell&) = delete;
    SocketShell& operator=(const SocketShell&) = delete;
    osal::IpEndpoint endpoint() const;
    bool shutdown_requested() const noexcept;
    // Diagnostics do not request framework shutdown. Counts saturate; the latest cause is retained.
    SocketShellStatus status() const;
    std::optional<Error> failure() const;
    void begin_shutdown();  // Stop accepting commands; keep delivering final output.
    void finish_shutdown(); // Join commands and drain replies with a bounded send deadline.
private:
#ifdef VINC_SHELL_TESTING
    friend struct SocketShellTestAccess;
#endif
    struct Impl;
    explicit SocketShell(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
