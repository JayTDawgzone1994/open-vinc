#pragma once

// Private bridge between module supervision, control sessions and the Message Bus.
// Application code receives Client from BaseModule instead of constructing its own transport.

#include <vinc/message_bus/message_bus.hpp>
#include <vinc/control/protocol.hpp>
#include <vinc/time.hpp>
namespace vinc::control { class Session; }
#include <functional>

namespace vinc::message_bus::detail {
struct Attachment;
constexpr std::uint16_t message_bus_domain = static_cast<std::uint16_t>(control::Domain::MessageBus);

// Host supplies ownership through these callbacks. The bus never opens a Socket,
// starts an I/O worker or becomes a second receiver of application frames.
struct ControlTransport {
    std::function<Result<void>(control::Frame)> send;
    std::function<void()> close;
};

class RuntimeAccess {
public:
    static vinc::Result<std::shared_ptr<Client>> create(std::shared_ptr<control::Session>);
    static vinc::Result<std::shared_ptr<Client>> create(ControlTransport);
    // Called only by the module's existing receive dispatcher; does not map/copy payload.
    static vinc::Result<void> receive(Client&, control::Frame);
    static void shutdown(Client&) noexcept;
    // Framework diagnostic snapshot. No mappings, payload or user callbacks.
    static std::vector<Attachment> inspect_attachments(Client&);
};
} // namespace vinc::message_bus::detail
