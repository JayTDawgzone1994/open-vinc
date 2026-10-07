#pragma once
// Linked only into the private fault fixture; production has no injection registry.
#include <vinc/osal/socket.hpp>
#include <vinc/osal/socket_poller.hpp>
namespace vinc::shell::test {
Result<osal::Socket> accept(osal::Socket& listener);
Result<std::size_t> wait(osal::SocketPoller& poller, std::span<osal::SocketReadyEvent> events,
                         vinc::Deadline deadline);
}
