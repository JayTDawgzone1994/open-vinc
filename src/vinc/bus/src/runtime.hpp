#pragma once

// Private bridge between module supervision, control sessions and the Bus.
// Application code receives Client from BaseModule instead of constructing its own transport.

#include <vinc/bus/bus.hpp>
#include <vinc/control/channel.hpp>
#include <functional>
namespace vinc::bus::detail {
struct Attachment;
constexpr std::uint16_t bus_domain = static_cast<std::uint16_t>(control::Domain::Bus);
class RuntimeAccess {
public:
    static std::shared_ptr<Client> create(std::shared_ptr<control::Session>);
    // Called only by the module's existing receive dispatcher; does not map/copy payload.
    static bool receive(Client&, control::Frame);
    static void shutdown(Client&) noexcept;
    // Framework diagnostic snapshot. No mappings, payload or user callbacks.
    static std::vector<Attachment> inspect_attachments(Client&);
};
class Broker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Broker(Limits = {});
    ~Broker();
    // Enqueue only; allocation/mapping runs on the broker worker, never supervisor.
    bool submit(std::uint64_t owner, std::string name, std::shared_ptr<control::Session>, control::Frame);
    void exited(std::uint64_t owner); // ONLY after Process::try_wait confirms exit
    bool drain_until(control::Deadline); // call after all producer processes have exited
};
}
