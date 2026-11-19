#pragma once
#include <vinc/module/context.hpp>

namespace vinc::detail {
// Designated framework integration seam, using the inherited Context and Host session.
// Keeping the returned handle alive enables dispatch; closing it does not join a callback.
struct ContextImageAccess {
    static Result<message_bus::detail::ImageSubscriber> subscribe(Context&, std::string topic,
        std::uint64_t schema_hash, std::function<void(message_bus::detail::ImageDelivery)> callback,
        message_bus::SubscriberOptions options = {});
};
}
