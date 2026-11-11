#include <vinc/shell/detail/network_params.hpp>
#include <vinc/module/detail/manager_access.hpp>
#include <vinc/osal/socket_address.hpp>
#include <vinc/param/error.hpp>
#include <iostream>

namespace vinc::shell {
namespace {
bool valid_ip(const std::string& ip) {
    return osal::IPv4Address::parse(ip).has_value() || osal::IPv6Address::parse(ip).has_value();
}

Error param_failure(const Error& cause) {
    return wrap_error(ErrorCode::ParamFailed, ErrorOperation::ConfigureNetwork, cause);
}

Result<void> register_setting(ModuleManager& manager, const std::string& name, param::Value defaults) {
    auto result = vinc::detail::ManagerAccess::register_param(manager, name, defaults, param::RestartHint::App);
    if (result || !result.error().is(module_system::ErrorCode::ParamFailed)) {
        return result;
    }

    // A timed-out registration can have committed. Only accept an identical
    // central declaration; do not silently change a different owner's contract.
    auto values = vinc::detail::ManagerAccess::read_params(manager, {"vinc." + name});
    if (values) {
        const auto& entry = values->at("vinc." + name);
        if (entry.default_value == defaults && entry.permission == param::Permission::Limited &&
            entry.restart_hint == param::RestartHint::App) {
            return {};
        }
    }

    return result;
}
}

Result<SocketShellOptions> NetworkParams::configure(SocketShellOptions options,
    std::optional<std::string> ip, std::optional<std::uint16_t> port) {
    std::lock_guard lock(mutex_);
    // Validate explicit input before making live mutations. Zero is intentional
    // OS-selected allocation; binding failure is not an invalid parameter.
    if (ip && !valid_ip(*ip)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::ConfigureNetwork});
    }

    if (!ip_registered_) {
        auto result = register_setting(manager_, "shell_ip", std::string(config::shell_socket_default_ip));
        if (!result) {
            return std::unexpected(param_failure(result.error()));
        }

        ip_registered_ = true;
    }

    if (!port_registered_) {
        auto result = register_setting(manager_, "shell_port", config::shell_socket_default_port);
        if (!result) {
            return std::unexpected(param_failure(result.error()));
        }

        port_registered_ = true;
    }

    if (ip) {
        auto result = vinc::detail::ManagerAccess::set_param(manager_, "shell_ip", *ip);
        if (!result) {
            return std::unexpected(param_failure(result.error()));
        }
    }

    if (port) {
        auto result = vinc::detail::ManagerAccess::set_param(manager_, "shell_port", *port);
        if (!result) {
            return std::unexpected(param_failure(result.error()));
        }
    }

    auto refreshed = refresh_locked();
    if (!refreshed) {
        return std::unexpected(refreshed.error());
    }

    if (!refreshed->empty()) {
        auto logged = vinc::detail::ManagerAccess::log_diagnostic(manager_, *refreshed);
        if (!logged) {
            // Diagnostics must not turn a successfully repaired setting into
            // an unavailable listener when Log itself is unavailable.
            std::cerr << *refreshed << '\n';
        }
    }

    options.ip = accepted_ip_;
    options.port = accepted_port_;
    return options;
}

Result<std::string> NetworkParams::refresh() {
    std::lock_guard lock(mutex_);
    if (!ip_registered_ || !port_registered_) {
        return std::string{};
    }

    return refresh_locked();
}

Result<std::string> NetworkParams::refresh_locked() {
    auto values = vinc::detail::ManagerAccess::read_params(manager_, {"vinc.shell_ip", "vinc.shell_port"});
    if (!values) {
        return std::unexpected(param_failure(values.error()));
    }

    auto ip = values->at("vinc.shell_ip").get<std::string>();
    auto port = values->at("vinc.shell_port").get<std::uint16_t>();
    if (!ip) {
        return std::unexpected(param_failure(ip.error()));
    }

    if (!port) {
        return std::unexpected(param_failure(port.error()));
    }

    accepted_port_ = *port;
    if (valid_ip(*ip)) {
        accepted_ip_ = *ip;
        return std::string{};
    }

    auto restored = vinc::detail::ManagerAccess::set_param(manager_, "shell_ip", accepted_ip_);
    if (!restored) {
        return std::unexpected(param_failure(restored.error()));
    }

    return "Socket Shell IP rejected; restored " + accepted_ip_ + "; not saved. Listener unchanged.";
}
}
