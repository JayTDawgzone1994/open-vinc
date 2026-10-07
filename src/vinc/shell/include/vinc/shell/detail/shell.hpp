#pragma once

// Framework-owned Shell with asynchronous commands and a foreground Tcl script.
// Central startup owns initialization and activation; this is not an application SDK API.
// Its exit path waits for module processes and Message Bus cleanup before returning.

#include <vinc/module/host/module_manager.hpp>
#include <vinc/shell/detail/socket_shell.hpp>
#include <filesystem>
#include <iosfwd>
#include <memory>

namespace vinc {
class Shell {
public:
    // Borrow manager/output for this Shell's entire lifetime. Initialize Tcl first.
    // Failed acquisition releases owned resources; reserved names remain manager-owned.
    static Result<std::unique_ptr<Shell>> create(ModuleManager& manager, std::ostream& output);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;
    // Reports terminal failure after the complete framework shutdown path.
    // Socket service failures are diagnosed and isolated; they do not request shutdown.
    // Individual command/script failures are displayed and leave the Shell running.
    Result<void> run(std::filesystem::path startup_script = {}, std::vector<std::string> script_args = {});
    // Bind before deployment; failures preserve the Socket/OSAL cause.
    Result<osal::IpEndpoint> start_socket_shell(shell::SocketShellOptions options = {});
    // Synchronous command entry. Does not parse Tcl. Nonzero application exit codes
    // are successful values; framework failure is unexpected(Error).
    Result<std::int32_t> process_line(const std::string& line);
    void request_shutdown();
    void wait_shutdown();

private:
#ifdef VINC_SHELL_TESTING
    friend struct ShellTestAccess;
#endif
    struct Impl;
    explicit Shell(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
