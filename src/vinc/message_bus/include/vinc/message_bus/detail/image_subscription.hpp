#pragma once
#include <vinc/message_bus/message_bus.hpp>

namespace vinc::message_bus::detail {
// Framework integration only. The immutable local image owns no shared Reader lease.
// Publisher identity is a Bus Publisher ID, not the Module MID.
struct ReceivedImage {
    message::Snapshot image;
    std::string topic;
    std::string publisher_name;
    std::uint64_t schema_hash = 0;
    std::uint64_t publisher_id = 0;
    std::uint64_t topic_count = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t monotonic_timestamp = 0;
    // Payload allocation retained by the adopted vector, excluding validator metadata.
    std::size_t payload_capacity = 0;
};
using ImageDelivery = std::unique_ptr<const ReceivedImage>;

class ImageAccess;
class ImageSubscriber {
    friend class ImageAccess;
    std::shared_ptr<SubscriberCore> core_;
    explicit ImageSubscriber(std::shared_ptr<SubscriberCore> core) : core_(std::move(core)) {
    }

public:
    ImageSubscriber(const ImageSubscriber&) = delete;
    ImageSubscriber& operator=(const ImageSubscriber&) = delete;
    ImageSubscriber(ImageSubscriber&&) noexcept = default;
    ImageSubscriber& operator=(ImageSubscriber&& other) noexcept {
        if (this != &other) {
            retire(core_);
            core_ = std::move(other.core_);
        }
        return *this;
    }
    ~ImageSubscriber() {
        retire(core_);
    }
    Result<void> close() {
        auto closing = std::move(core_);
        return close_confirmed(closing);
    }
    SubscriberStats stats() const {
        return detail::stats(core_);
    }
    Result<ImageDelivery> get_last();
};

class ImageAccess {
public:
    static Result<ImageSubscriber> subscribe(Client&, std::string topic, std::uint64_t schema_hash,
                                             SubscriberOptions options = {});
    static const std::shared_ptr<SubscriberCore>& core(const ImageSubscriber& subscriber) {
        return subscriber.core_;
    }
};
}
