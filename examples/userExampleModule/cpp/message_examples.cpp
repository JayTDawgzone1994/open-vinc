#include <vinc/time.hpp>
#include "message_examples.hpp"

#include <iostream>
#include <vector>

namespace user_example {
vinc::Result<void> fill(vinc::UserExampleFrame& message, std::uint64_t sequence) {
    auto& fields = message.data();
    fields.sequence = sequence;
    auto named = fields.note.assign("SDK walkthrough");
    if (!named) {
        return named;
    }

    auto samples = fields.samples.copy_from(std::vector<std::int32_t>{10, 20, 30});
    if (!samples) {
        return samples;
    }

    auto flags = fields.flags.copy_from(std::vector<bool>{true, false});
    if (!flags) {
        return flags;
    }

    auto resized = fields.items.resize(1);
    if (!resized) {
        return resized;
    }

    // [] is cheap borrowed access. resize succeeded, so index zero is valid.
    // A nested element is a proxy value, not an owning reference or deep copy.
    auto item = fields.items[0];
    item.data().id = 7;
    item.data().score = 0.95;
    auto label = item.data().label.assign("camera");
    if (!label) {
        return label;
    }

    auto groups = fields.groups.resize(1);
    if (!groups) {
        return groups;
    }

    return fields.groups[0].assign(fields.items);
}

void describe(const vinc::UserExampleFrame& message) {
    std::cout << "MESSAGE sequence=" << message.data().sequence.get()
              << " topic=" << message.topic_name()
              << " publisher=" << message.publisher_name()
              << " publisher_id=" << message.publisher_instance()
              << " count=" << message.get_topic_count()
              << " timestamp_ns=" << message.get_message_timestamp()
                      << " utc=" << vinc::format_utc(message.get_message_timestamp())
                      << " monotonic_ns=" << message.get_message_monotonic_timestamp()
              << " samples=" << message.data().samples.size()
              << " note=" << message.data().note.str() << '\n';
    if (!message.data().groups.empty() && !message.data().groups[0].empty()) {
        auto item = message.data().groups[0][0];
        std::cout << "NESTED id=" << item.data().id.get() << " label=" << item.data().label.str() << '\n';
    }
}

vinc::Result<void> demonstrate_storage() {
    auto message = vinc::make_message<vinc::UserExampleFrame>();
    auto filled = fill(*message, 42);
    if (!filled) {
        return filled;
    }

    auto& samples = message->data().samples;
    const std::uint64_t reserved_samples = 8;
    auto reserved = samples.reserve(reserved_samples);
    if (!reserved) {
        return reserved;
    }

    samples[0] = 11; // Guaranteed in range; contract violations terminate the process.
    auto updated = samples.set(1, 22);
    if (!updated) {
        return updated;
    }

    auto appended = samples.push_back(40);
    if (!appended) {
        return appended;
    }

    auto emplaced = samples.emplace_back(50);
    if (!emplaced) {
        return emplaced;
    }

    auto removed = samples.pop_back();
    if (!removed) {
        return removed;
    }

    auto shrunk = samples.shrink_to_fit();
    if (!shrunk) {
        return shrunk;
    }

    auto native = samples.to_vector();
    if (!native) {
        return std::unexpected(native.error());
    }

    std::cout << "ARRAY size=" << samples.size() << " capacity=" << samples.capacity()
              << " empty=" << samples.empty() << " values=";
    for (auto value : samples) {
        std::cout << static_cast<std::int32_t>(value) << ' ';
    }
    std::cout << '\n';

    // Owning array copies are independent; copying a proxy only copies the borrow.
    auto independent_array = samples;
    auto cleared = independent_array.clear();
    if (!cleared) {
        return cleared;
    }

    auto assigned = independent_array.assign(samples);
    if (!assigned) {
        return assigned;
    }

    auto item = message->data().items[0];
    auto alias = item;
    alias.data().score = 0.5;
    auto independent_item = item.copy();
    if (!independent_item) {
        return std::unexpected(independent_item.error());
    }

    independent_item->data().id = 8;
    auto replaced = message->data().items.set(0, *independent_item);
    if (!replaced) {
        return replaced;
    }

    // at() is the recoverable path for an untrusted index, including nested copies.
    auto checked = message->data().items.at(0);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    auto invalid = samples.at(samples.size());
    if (!invalid) {
        if (!invalid.error().is(vinc::message::ErrorCode::OutOfRange)) {
            return std::unexpected(invalid.error());
        }

        std::cout << "EXPECTED checked access: " << vinc::error_to_string(invalid.error()) << '\n';
    }

    // String payloads preserve bytes; they do not implicitly validate UTF-8.
    const std::string embedded_zero("a\0b", 3);
    auto text = message->data().note.assign(embedded_zero);
    if (!text) {
        return text;
    }

    auto compacted = message->compact();
    if (!compacted) {
        return compacted;
    }

    auto usage = message->usage();
    if (!usage) {
        return std::unexpected(usage.error());
    }

    // Serialization is a payload image, not a saved publisher identity.
    std::vector<std::uint8_t> image;
    auto encoded = message->serialize(image);
    if (!encoded) {
        return encoded;
    }

    auto decoded = vinc::make_message<vinc::UserExampleFrame>();
    auto restored = decoded->deserialize(image);
    if (!restored) {
        return restored;
    }

    auto clone = *decoded;
    auto copied = clone.assign(*message);
    if (!copied) {
        return copied;
    }

    auto snapshot = message->snapshot();
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }

    auto from_image = vinc::UserExampleFrame::from_bytes(snapshot->bytes());
    if (!from_image) {
        return std::unexpected(from_image.error());
    }

    std::cout << "STORAGE_OK type=" << message->msg_name_str() << " hash=" << message->type_hash()
              << " protocol=" << vinc::UserExampleFrame::protocol
              << " compact_bytes=" << usage->compact_bytes
              << " allocated_bytes=" << usage->allocated_bytes
              << " limit_bytes=" << usage->limit_bytes << '\n';
    return {};
}
}
