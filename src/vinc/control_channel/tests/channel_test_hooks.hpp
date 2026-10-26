#pragma once

// Compiled only into control_channel_fault_tests. No production target includes
// this header or calls these functions; the wire protocol and public API stay intact.
#include <vinc/osal/socket_poller.hpp>

namespace vinc::control::test {
Result<std::size_t> try_send(osal::Socket& socket, std::span<const std::byte> bytes);
Result<std::size_t> wait_until(osal::SocketPoller& poller, std::span<osal::SocketReadyEvent> ready,
                               Deadline deadline, std::stop_token stop);
}
