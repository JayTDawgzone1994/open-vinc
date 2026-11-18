#include <vinc/module/detail/context_image_access.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc_messages/Frame.hpp>
#include <optional>
#include <stdexcept>

namespace {
using namespace vinc;
using Images = detail::ContextImageAccess;
using Delivery = message_bus::detail::ImageDelivery;
constexpr std::uint32_t retained_slots = 4;
constexpr std::uint64_t payload_limit = 8192;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
template<class T> auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}
std::uint64_t sequence(const message_bus::detail::ReceivedImage& value) {
    return take(Frame::from_bytes(value.image.bytes()))->data().sequence.get();
}
void publish(message_bus::Publisher<Frame>& publisher, std::uint64_t sequence) {
    auto value = make_message<Frame>();
    value->data().sequence = sequence;
    take(publisher.push(*value));
}
}

// Runs in a real Plugin; all registration, notifications and ACKs cross the Host seam.
void verify_context_images(vinc::Context& context, std::string_view mode) {
    const std::string topic = "images/frame";
    auto publisher = take(context.advertise<Frame>(topic,
        {.slots = retained_slots, .max_message_bytes = payload_limit}));
    std::vector<std::uint64_t> received;
    Delivery saved;
    auto collect = [&](Delivery value) {
        check(value->topic == topic && value->schema_hash == Frame::schema().hash &&
              value->publisher_name == "contextNode" && value->publisher_id != 0 &&
              value->timestamp != 0 && value->monotonic_timestamp != 0 &&
              value->payload_capacity >= value->image.bytes().size(), "owned image metadata");
        received.push_back(sequence(*value));
        saved = std::move(value);
    };
    auto invalid = Images::subscribe(context, topic, 0, collect);
    check(!invalid && invalid.error().is(module_system::ErrorCode::BusFailed), "zero hash rejected");
    auto empty_callback = Images::subscribe(context, topic, Frame::schema().hash, {});
    check(!empty_callback && empty_callback.error().is(module_system::ErrorCode::InvalidArgument),
          "empty callback rejected before registration");

    if (mode == "image-batch") {
        auto second = take(context.advertise<Frame>(topic,
            {.slots = retained_slots, .max_message_bytes = payload_limit}));
        auto subscriber = take(Images::subscribe(context, topic, Frame::schema().hash, [&](Delivery value) {
            collect(std::move(value));
            if (received.size() == 1) {
                publish(publisher, 3);
            }
        }));
        publish(publisher, 1);
        publish(second, 2);
        check(take(context.pumpOnce()).callbacks == 2 && received == std::vector<std::uint64_t>{1, 2},
              "image batch extends beyond captured multi-Publisher boundary");
        auto last = take(subscriber.get_last());
        check(sequence(*last) == 3 && sequence(*saved) == 2, "latest mutated retained image");
        check(take(context.pumpOnce()).callbacks == 1 && received.back() == 3,
              "get_last consumed callback cursor");
        const auto saved_count = saved->topic_count;
        const auto saved_time = saved->timestamp;
        // Keeping an image across complete ring reuse must not keep a shared lease.
        for (std::uint64_t index = 4; index <= 12; ++index) {
            publish(publisher, index);
        }
        check(sequence(*saved) == 3 && saved->topic_count == saved_count && saved->timestamp == saved_time,
              "ring overwrite changed owned image");
        take(subscriber.close());
        take(publisher.close());
        take(second.close());
        auto canonical = take(message::detail::FrameworkAccess::canonical_image(saved->image, payload_limit));
        check(take(Frame::from_bytes(canonical.bytes()))->data().sequence.get() == 3,
              "retired source invalidated canonical conversion");
        check(take(context.pumpOnce()).callbacks == 0, "closed image callback remained active");
        return;
    }

    if (mode == "image-new-only") {
        publish(publisher, 1);
        auto subscriber = take(Images::subscribe(context, topic, Frame::schema().hash, collect,
            {.start = message_bus::StartPosition::NewOnly}));
        check(take(context.pumpOnce()).callbacks == 0, "NewOnly replayed image history");
        publish(publisher, 2);
        check(take(context.pumpOnce()).callbacks == 1 && received.back() == 2, "NewOnly missed new image");
        take(subscriber.close());
        auto replacement = take(Images::subscribe(context, topic, Frame::schema().hash, collect,
            {.start = message_bus::StartPosition::NewOnly}));
        publish(publisher, 3);
        check(take(context.pumpOnce()).callbacks == 1 && received.back() == 3, "resubscribe after ACK");
        check(replacement.stats().lost_messages == 0, "NewOnly counted intentional skip as loss");
        return;
    }

    if (mode == "image-loss") {
        auto subscriber = take(Images::subscribe(context, topic, Frame::schema().hash, collect));
        for (std::uint64_t index = 1; index <= retained_slots + 2; ++index) {
            publish(publisher, index);
        }
        auto batch = take(context.pumpOnce());
        check(batch.callbacks == retained_slots && batch.lost_messages == 2 &&
              subscriber.stats().lost_messages == 2 && received.front() == 3, "image loss counters");
        check(take(context.pumpOnce()).lost_messages == 0, "image loss counted twice");
        return;
    }

    if (mode == "image-oversized") {
        auto subscriber = take(Images::subscribe(context, topic, Frame::schema().hash, collect,
            {.max_message_bytes = 1}));
        publish(publisher, 1);
        auto latest = subscriber.get_last();
        check(!latest && latest.error().is(message_bus::ErrorCode::CapacityExceeded), "oversized latest");
        check(take(context.pumpOnce()).callbacks == 0 && subscriber.stats().oversized_messages == 1,
              "oversized image was not skipped");
        return;
    }

    if (mode == "image-cancel") {
        std::optional<message_bus::detail::ImageSubscriber> subscriber;
        subscriber.emplace(take(Images::subscribe(context, topic, Frame::schema().hash, [&](Delivery value) {
            collect(std::move(value));
            take(subscriber->close());
        })));
        publish(publisher, 1);
        publish(publisher, 2);
        check(take(context.pumpOnce()).callbacks == 1, "self-close did not end image dispatch");
        check(take(context.pumpOnce()).callbacks == 0 && sequence(*saved) == 1, "closed image lifetime");
        return;
    }

    if (mode == "image-error") {
        auto subscriber = take(Images::subscribe(context, topic, Frame::schema().hash, [](Delivery) {
            throw std::runtime_error("intentional image callback failure");
        }));
        publish(publisher, 1);
        auto result = context.pumpOnce();
        check(!result && result.error().is(module_system::ErrorCode::HookException), "image exception classification");
        return;
    }
    throw std::runtime_error("unknown image test mode");
}
