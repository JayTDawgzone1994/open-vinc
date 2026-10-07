#pragma once

// Configuration data only. Central framework startup owns the Socket Shell service.
#include <vinc/config.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vinc::shell {
struct SocketShellOptions {
    std::string ip = config::shell_socket_default_ip;
    std::uint16_t port = config::shell_socket_default_port;
    std::chrono::milliseconds disconnect_timeout{config::shell_socket_disconnect_timeout};
    std::chrono::milliseconds send_timeout{config::shell_socket_send_timeout};
    std::size_t max_connections = config::shell_socket_max_connections;
};
}
