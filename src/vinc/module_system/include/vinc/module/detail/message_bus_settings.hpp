#pragma once

#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/param/detail/manager.hpp>

namespace vinc::detail {
// Framework-owned consumer, not a Param validator registry. Initialize after
// loading persistence and before admitting Modules. Only the Bus worker reads
// thereafter; destroying that worker must precede this object and ParamManager.
class MessageBusSettings {
public:
    MessageBusSettings(param::detail::ParamManager&, message_bus::Limits);
    Result<void> initialize();
    Result<message_bus::detail::PublisherDefaults> read_publisher_defaults();
    Result<std::uint32_t> read_topic_limit();

private:
    param::detail::ParamManager& params_;
    message_bus::Limits limits_;
    message_bus::detail::PublisherDefaults effective_;
    bool slots_registered_ = false;
    bool bytes_registered_ = false;
    bool topic_limit_registered_ = false;

    Result<void> register_publisher_defaults();
    Result<void> register_topic_limit();
    Result<void> restore(const param::ParamMap&);
};
}
