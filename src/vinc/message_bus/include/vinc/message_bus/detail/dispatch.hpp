#pragma once
#include <vinc/message_bus/message_bus.hpp>
#include <vinc/message_bus/detail/image_subscription.hpp>
#include <vector>

namespace vinc::message_bus::detail {
// Local metadata only. Capturing never holds shared reader protection across user code.
struct ReadBoundary {
    std::uint64_t publisher;
    std::uint64_t upper;
};
using ReadBatch = std::vector<ReadBoundary>;
Result<ReadBatch> capture(const std::shared_ptr<SubscriberCore>&);
Result<std::unique_ptr<BaseMessage>> read_batch(const std::shared_ptr<SubscriberCore>&,
    const ReadBatch&, const message::detail::Schema&, Factory);
Result<ImageDelivery> read_image_batch(const std::shared_ptr<SubscriberCore>&, const ReadBatch&);
bool closed(const std::shared_ptr<SubscriberCore>&) noexcept;
Result<void> enable_notifications(const std::shared_ptr<SubscriberCore>&);

struct Attachment;
std::vector<Attachment> inspect_sources(const std::shared_ptr<SubscriberCore>&);

class DispatchAccess {
public:
    static std::shared_ptr<SubscriberCore> core(const ImageSubscriber& subscriber) {
        return ImageAccess::core(subscriber);
    }
    template <Message T>
    static std::shared_ptr<SubscriberCore> core(const Subscriber<T>& subscriber) {
        return subscriber.core_;
    }
};
}
