#pragma once

#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/param/detail/manager.hpp>

namespace vinc::detail {
// Framework-owned consumer, not a Param validator registry. Initialize after
// loading persistence and before admitting Modules. Only the Bus worker reads
// thereafter; destroying that worker must precede this object and ParamManager.
class PublisherDefaults {
public:
    PublisherDefaults(param::detail::ParamManager&, message_bus::Limits);
    Result<void> initialize();
    Result<message_bus::detail::PublisherDefaults> read();

private:
    param::detail::ParamManager& params_;
    message_bus::Limits limits_;
    message_bus::detail::PublisherDefaults effective_;
    bool slots_registered_ = false;
    bool bytes_registered_ = false;

    Result<void> restore(const param::ParamMap&);
};
}
