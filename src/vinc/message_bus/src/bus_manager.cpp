// Central Message Bus registry and shared-memory owner. One worker serializes topology changes
// and process-exit cleanup; payload copies never pass through this BusManager.

#include <vinc/time.hpp>
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/message_bus/detail/diagnostics_protocol.hpp>
#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/control/channel.hpp>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#ifdef VINC_MESSAGE_BUS_TESTING
#include "bus_test_hooks.hpp"
#endif

namespace vinc::message_bus::detail {
namespace {
vinc::Error io_resource(const vinc::Error& error, ErrorOperation operation) {
    return vinc::wrap_error(ErrorCode::Io, operation, error);
}
}
struct BusManager::Impl {
    struct Peer {
        std::shared_ptr<control::Session> session;
        std::uint64_t last_request = 0;
        std::string notification_name;
    };

    struct Sub {
        std::uint64_t id;
        std::uint64_t owner;
        std::uint32_t index;
        bool notifications = false;
    };

    // A topic shares its schema and count, but each Publisher owns a separate ring buffer.
    // Adding another Publisher does not relocate memory already mapped by existing clients.
    struct Topic {
        std::uint64_t schema = 0;
        std::shared_ptr<NamedCounter> counter;
        std::map<std::uint64_t, std::shared_ptr<RingBuffer>> publishers;
        std::map<std::uint64_t, Sub> subscribers;
        std::vector<bool> used;
        // Preserve retired reader/source losses while this topic still exists.
        std::uint64_t lost = 0;
        std::uint64_t overwrites = 0;
        std::uint64_t oversized = 0;
    };

    struct Work {
        std::uint64_t owner;
        std::string name;
        std::shared_ptr<control::Session> session;
        control::Frame frame;
        bool exit = false;
        std::string notification_name;
    };

    std::shared_ptr<vinc::detail::ResourceNames> resources;
    Limits limits;
    std::uint64_t next_id = 1;
    std::uint64_t allocated = 0;
    std::map<std::string, Topic> topics;
    std::map<std::uint64_t, Peer> peers;
    // Only the worker touches topics/peers/allocated. This mutex protects the work
    // queue, stopping flag and pending_work; it is released before OS or Socket work.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Work> work;
    bool stopping = false;
    std::size_t pending_work = 0;
    std::thread worker;

    explicit Impl(Limits settings, std::shared_ptr<vinc::detail::ResourceNames> names)
        : resources(std::move(names)), limits(settings) {
    }

    void start_worker() {
        worker = std::thread([this] {
            run();
        });
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            cv.notify_all();
        }

        if (worker.joinable()) {
            worker.join();
        }
    }

    void send(std::uint64_t owner, control::Frame frame) {
        auto peer = peers.find(owner);
        if (peer == peers.end()) {
            return;
        }

        if (!peer->second.session->send(std::move(frame))) {
            peer->second.session->close();
        }
    }

    void response(const Work& item, vinc::Result<control::Bytes> body) {
        control::Frame frame{control::Kind::Response, message_bus_domain, item.frame.operation,
                             item.frame.request};
        if (body) {
            control::put_u32(frame.payload, result_success);
            frame.payload.insert(frame.payload.end(), body->begin(), body->end());
        } else {
            frame.payload = encode_failure(body.error());
        }

        if (item.frame.operation == ListTopicConnections) {
            // A congested diagnostic reply may time out at its caller; it must
            // not turn a read-only refresh into a forced Module disconnection.
            (void)item.session->send(std::move(frame));
            return;
        }

        send(item.owner, std::move(frame));
    }

    vinc::Result<Topic*> topic(const std::string& name, std::uint64_t schema) {
        if (name.empty() || name.size() > maximum_name_bytes || name.find('\0') != std::string::npos ||
            !schema) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::CreateTopic));
        }

        auto it = topics.find(name);
        if (it != topics.end()) {
            if (it->second.schema != schema) {
                return std::unexpected(vinc::make_error(ErrorCode::TypeMismatch, ErrorOperation::CreateTopic));
            }

            return &it->second;
        }

        if (topics.size() >= limits.max_topics || allocated > limits.max_shared_bytes - sizeof(SharedWord)) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::CreateTopic));
        }

        auto counter_name = resources ? resources->reserve() : vinc::Result<std::string>(shared_name('t'));
        if (!counter_name) {
            return std::unexpected(io_resource(counter_name.error(), ErrorOperation::CreateTopic));
        }
        auto counter = NamedCounter::create(*counter_name, resources);
        if (!counter) {
            return std::unexpected(counter.error());
        }
        Topic topic_state;
        topic_state.schema = schema;
        topic_state.counter = std::move(*counter);
        topic_state.used.resize(limits.subscribers_per_topic);
        auto [added, _] = topics.emplace(name, std::move(topic_state));
        allocated += sizeof(SharedWord);
        return &added->second;
    }

    void prune() {
        for (auto it = topics.begin(); it != topics.end();) {
            if (it->second.publishers.empty() && it->second.subscribers.empty()) {
                allocated -= sizeof(SharedWord);
                it = topics.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Notification has no ownership of the retired ring. A failed notification
    // closes only its recipient and cannot interrupt another topic's cleanup.
    void notify_detached(const Sub& subscriber, std::uint64_t publisher) noexcept {
        try {
#ifdef VINC_MESSAGE_BUS_TESTING
            testing::checkpoint(testing::FaultPoint::DetachEncode);
#endif
            control::Frame frame{control::Kind::Event, message_bus_domain, Detach};
            control::put_u64(frame.payload, subscriber.id);
            control::put_u64(frame.payload, publisher);
#ifdef VINC_MESSAGE_BUS_TESTING
            testing::checkpoint(testing::FaultPoint::DetachSend);
#endif
            send(subscriber.owner, std::move(frame));
        } catch (...) {
            auto peer = peers.find(subscriber.owner);
            if (peer != peers.end()) {
                peer->second.session->close();
            }
        }
    }

    static void collect_reader(Topic& topic_state, RingBuffer& ring, std::uint32_t index) {
        const auto& reader = ring.reader(index);
        topic_state.lost += shared_load(reader.lost_messages);
        topic_state.overwrites += shared_load(reader.overwrite_events);
        topic_state.oversized += shared_load(reader.oversized_messages);
    }

    void remove_publisher(Topic& topic_state,
                          std::map<std::uint64_t, std::shared_ptr<RingBuffer>>::iterator it) {
        auto ring_buffer = it->second;
        const auto publisher = it->first;
        ring_buffer->retire();
        for (const auto& [_, subscriber] : topic_state.subscribers) {
            collect_reader(topic_state, *ring_buffer, subscriber.index);
        }
        allocated -=
            RingBuffer::control_bytes(ring_buffer->descriptor.slots, ring_buffer->descriptor.readers) +
            ring_buffer->descriptor.slots * RingBuffer::slot_stride(ring_buffer->descriptor.bytes);
        topic_state.publishers.erase(it);
        for (const auto& [_, subscriber] : topic_state.subscribers) {
            notify_detached(subscriber, publisher);
        }
    }

    // Deduplicate callback destinations by their central-approved process generation.
    control::Bytes notification_routes(const Topic& topic_state, std::uint64_t enabling = 0) const {
        std::map<std::uint64_t, std::string> destinations;
        for (const auto& [_, subscriber] : topic_state.subscribers) {
            auto peer = peers.find(subscriber.owner);
            if ((subscriber.notifications || subscriber.id == enabling) &&
                peer != peers.end() && !peer->second.notification_name.empty()) {
                destinations.emplace(subscriber.owner, peer->second.notification_name);
            }
        }
        control::Bytes bytes;
        control::put_u32(bytes, static_cast<std::uint32_t>(destinations.size()));
        for (const auto& [owner, name] : destinations) {
            control::put_u64(bytes, owner);
            // Names originate in central resource admission and have a 24-byte bound.
            (void)control::put_string(bytes, name);
        }
        return bytes;
    }

    void update_notification_routes(const Topic& topic_state) noexcept {
        for (const auto& [id, publisher] : topic_state.publishers) {
            try {
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::checkpoint(testing::FaultPoint::NotificationRoutesEncode);
#endif
                const auto routes = notification_routes(topic_state);
                control::Frame frame{control::Kind::Event, message_bus_domain, NotificationRoutesChanged};
                control::put_u64(frame.payload, id);
                frame.payload.insert(frame.payload.end(), routes.begin(), routes.end());
                send(publisher->descriptor.owner, std::move(frame));
            } catch (...) {
                // Match Detach's existing topology-failure boundary: a recipient
                // that cannot receive consistent routes is disconnected, while
                // confirmed-exit reader reclamation continues for other owners.
                auto peer = peers.find(publisher->descriptor.owner);
                if (peer != peers.end()) {
                    peer->second.session->close();
                }
            }
        }
    }

    Result<control::Bytes> enable_notifications(const Work& item) {
        control::Reader reader(item.frame.payload);
        const auto id = reader.u64();
        if (!reader.done()) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EnableNotifications});
        }
        for (auto& [_, topic_state] : topics) {
            auto found = topic_state.subscribers.find(id);
            if (found != topic_state.subscribers.end() && found->second.owner == item.owner) {
                const auto routes = notification_routes(topic_state, id);
                // Reserve the largest encoded Descriptor, so subscriber-first
                // registration also fits a later Advertise response. This is a
                // wire ceiling, separate from the configured reader capacity.
                constexpr auto descriptor_fixed_bytes = 5 * sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t);
                constexpr auto descriptor_text_bytes = 4 * (sizeof(std::uint32_t) + maximum_name_bytes);
                constexpr auto route_budget = control::max_payload - response_status_bytes -
                                              descriptor_fixed_bytes - descriptor_text_bytes;
                if (routes.size() > route_budget) {
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::EnableNotifications});
                }

                found->second.notifications = true;
                update_notification_routes(topic_state);
                return control::Bytes{};
            }
        }
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::EnableNotifications});
    }

    // Called only after a quiescent unsubscribe or confirmed owner-process exit.
    // Clearing a live reader lease based only on a timeout would permit unsafe overwrite.
    void remove_subscriber(Topic& topic_state, std::map<std::uint64_t, Sub>::iterator it) {
        for (auto& [_, publisher] : topic_state.publishers) {
            collect_reader(topic_state, *publisher, it->second.index);
            publisher->reset_reader(it->second.index);
        }

        topic_state.used[it->second.index] = false;
        const bool notifications = it->second.notifications;
        topic_state.subscribers.erase(it);
        if (notifications) {
            update_notification_routes(topic_state);
        }
    }

    void exited(std::uint64_t owner) {
        // The owner is a unique process generation, never a reusable OS PID.
        for (auto& [_, topic_state] : topics) {
            for (auto entry = topic_state.subscribers.begin(); entry != topic_state.subscribers.end();) {
                if (entry->second.owner == owner) {
                    auto removed = entry++;
                    remove_subscriber(topic_state, removed);
                } else {
                    ++entry;
                }
            }

            for (auto entry = topic_state.publishers.begin(); entry != topic_state.publishers.end();) {
                if (entry->second->descriptor.owner == owner) {
                    auto removed = entry++;
                    remove_publisher(topic_state, removed);
                } else {
                    ++entry;
                }
            }
        }

        peers.erase(owner);
        prune();
    }

    // Run on the BusManager worker. Validate schema and aggregate memory limits before
    // creating resources, then notify subscriptions of the resulting topology change.
    vinc::Result<control::Bytes> advertise(const Work& item) {
        control::Reader reader(item.frame.payload);
        control::Bytes body;
        const auto name = reader.string();
        const auto schema = reader.u64();
        const auto requested = reader.u32();
        const auto bytes = reader.u64();
        if (!reader.done() || !requested || requested > maximum_ring_buffer_slots ||
            bytes < minimum_message_bytes || bytes > SIZE_MAX - payload_alignment_mask ||
            bytes > limits.max_shared_bytes || item.name.size() > maximum_name_bytes) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise));
        }

        const auto slots = (std::max)(minimum_ring_buffer_slots, requested);
        if (!valid_topic_separators(name)) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise));
        }

        if (RingBuffer::slot_stride(bytes) > limits.max_shared_bytes / slots) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Advertise));
        }

        const auto data_size = slots * RingBuffer::slot_stride(bytes);
        const auto metadata_size = RingBuffer::control_bytes(slots, limits.subscribers_per_topic);
        if (data_size > UINT64_MAX - metadata_size) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Advertise));
        }

        const auto needed = data_size + metadata_size;
        if (needed > limits.max_shared_bytes || allocated > limits.max_shared_bytes - needed) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Advertise));
        }

        auto found = topic(name, schema);
        if (!found) {
            return std::unexpected(found.error());
        }

        auto& topic_state = **found;
        // Creating a new topic consumes its counter budget; recheck after topic().
        if (topic_state.publishers.size() >= limits.publishers_per_topic || next_id == UINT64_MAX ||
            allocated > limits.max_shared_bytes - needed) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Advertise));
        }
        Descriptor descriptor;
        descriptor.id = next_id++;
        descriptor.owner = item.owner;
        descriptor.rank = descriptor.id;
        descriptor.schema = schema;
        descriptor.slots = slots;
        descriptor.bytes = bytes;
        descriptor.readers = limits.subscribers_per_topic;
        auto control_name = resources ? resources->reserve() : vinc::Result<std::string>(shared_name('c'));
        if (!control_name) {
            return std::unexpected(io_resource(control_name.error(), ErrorOperation::Advertise));
        }
        auto data_name = resources ? resources->reserve() : vinc::Result<std::string>(shared_name('d'));
        if (!data_name) {
            if (resources) {
                (void)resources->retire(*control_name);
            }
            return std::unexpected(io_resource(data_name.error(), ErrorOperation::Advertise));
        }
        descriptor.control = std::move(*control_name);
        descriptor.data = std::move(*data_name);
        descriptor.counter = topic_state.counter->name;
        descriptor.publisher = item.name;
        auto ring_buffer = RingBuffer::create(descriptor, resources);
        if (!ring_buffer) {
            return std::unexpected(ring_buffer.error());
        }

        auto encoded = encode_descriptor(body, descriptor);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto routes = notification_routes(topic_state);
        body.insert(body.end(), routes.begin(), routes.end());
        if (body.size() + response_status_bytes > control::max_payload) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Advertise});
        }

        (*ring_buffer)->requested_slots = requested;
        topic_state.publishers.emplace(descriptor.id, *ring_buffer);
        allocated += needed;
        for (const auto& [_, sub] : topic_state.subscribers) {
            control::Frame frame{control::Kind::Event, message_bus_domain, Attach};
            control::put_u64(frame.payload, sub.id);
            auto attachment = encode_attachment(frame.payload, {descriptor, sub.index, 0});
            if (!attachment) {
                auto peer = peers.find(sub.owner);
                if (peer != peers.end()) {
                    peer->second.session->close();
                }
                continue;
            }
            send(sub.owner, std::move(frame));
        }

        return body;
    }

    vinc::Result<control::Bytes> subscribe(const Work& item) {
        control::Reader reader(item.frame.payload);
        control::Bytes body;
        const auto name = reader.string();
        const auto schema = reader.u64();
        const auto start = reader.u32();
        if (!reader.done() || start > 1) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Subscribe));
        }

        if (!valid_topic_separators(name)) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Subscribe));
        }

        auto found = topic(name, schema);
        if (!found) {
            return std::unexpected(found.error());
        }

        auto& topic_state = **found;
        auto free_reader = std::find(topic_state.used.begin(), topic_state.used.end(), false);
        if (free_reader == topic_state.used.end() || next_id == UINT64_MAX) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Subscribe));
        }

        const auto index = static_cast<std::uint32_t>(free_reader - topic_state.used.begin());
        const auto id = next_id++;
        control::put_u64(body, id);
        control::put_u32(body, static_cast<std::uint32_t>(topic_state.publishers.size()));
        for (const auto& [_, publisher] : topic_state.publishers) {
            auto attachment = encode_attachment(
                body, {publisher->descriptor, index, start ? shared_load(publisher->header().head) : 0});
            if (!attachment) {
                return std::unexpected(attachment.error());
            }
        }

        if (body.size() + response_status_bytes > control::max_payload) {
            return std::unexpected(
                vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Subscribe));
        }

        topic_state.subscribers.emplace(id, Sub{id, item.owner, index});
        *free_reader = true;
        return body;
    }

    vinc::Result<control::Bytes> close_registration(const Work& item) {
        control::Reader reader(item.frame.payload);
        control::Bytes body;
        const auto id = reader.u64();
        if (!reader.done()) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::CloseRegistration));
        }

        for (auto& [_, topic_state] : topics) {
            if (item.frame.operation == ClosePublisher) {
                auto publisher = topic_state.publishers.find(id);
                if (publisher != topic_state.publishers.end() &&
                    publisher->second->descriptor.owner == item.owner) {
                    remove_publisher(topic_state, publisher);
                    break;
                }
            } else {
                auto subscriber = topic_state.subscribers.find(id);
                if (subscriber != topic_state.subscribers.end() && subscriber->second.owner == item.owner) {
                    remove_subscriber(topic_state, subscriber);
                    break;
                }
            }
        }

        return body;
    }

    vinc::Result<control::Bytes> list_topics(const Work& item) {
        control::Reader reader(item.frame.payload);
        const auto after = reader.string();
        if (!reader.done() || after.size() > maximum_name_bytes) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopics});
        }
        std::vector<TopicInfo> page;
        auto iterator = topics.upper_bound(after);
        while (iterator != topics.end() && page.size() < topics_per_page) {
            if (!iterator->second.publishers.empty()) {
                page.push_back({iterator->first, iterator->second.schema,
                    static_cast<std::uint32_t>(iterator->second.publishers.size())});
            }
            ++iterator;
        }
        control::Bytes result;
        control::put_u32(result, static_cast<std::uint32_t>(page.size()));
        for (const auto& entry : page) {
            auto encoded = control::put_string(result, entry.name);
            if (!encoded) {
                return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::ListTopics, encoded.error()));
            }
            control::put_u64(result, entry.schema);
            control::put_u32(result, entry.publishers);
        }
        const std::string next = iterator != topics.end() && !page.empty() ? page.back().name : std::string{};
        auto encoded = control::put_string(result, next);
        if (!encoded) {
            return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::ListTopics, encoded.error()));
        }
        return result;
    }

    vinc::Result<control::Bytes> inspect_topics(const Work& item) {
        control::Reader reader(item.frame.payload);
        const auto after = reader.string();
        if (!reader.done() || after.size() > maximum_name_bytes || after.find('\0') != std::string::npos) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::InspectTopics});
        }

        TopicDiagnosticsPage page;
        auto iterator = topics.upper_bound(after);
        while (iterator != topics.end() && page.topics.size() < diagnostics_per_page) {
            const auto& [name, topic] = *iterator;
            TopicDiagnostics entry;
            entry.name = name;
            entry.schema = topic.schema;
            entry.subscribers = static_cast<std::uint32_t>(topic.subscribers.size());
            entry.lost_messages = topic.lost;
            entry.overwrite_events = topic.overwrites;
            entry.oversized_messages = topic.oversized;
            for (const auto& [id, ring] : topic.publishers) {
                entry.publishers.push_back({id, ring->descriptor.owner, ring->descriptor.publisher,
                    ring->stats(ring->requested_slots)});
                for (const auto& [_, subscriber] : topic.subscribers) {
                    const auto& shared = ring->reader(subscriber.index);
                    entry.lost_messages += shared_load(shared.lost_messages);
                    entry.overwrite_events += shared_load(shared.overwrite_events);
                    entry.oversized_messages += shared_load(shared.oversized_messages);
                }
            }

            page.topics.push_back(std::move(entry));
            ++iterator;
        }

        if (iterator != topics.end() && !page.topics.empty()) {
            page.next = page.topics.back().name;
        }

        return encode_diagnostics(page);
    }

    vinc::Result<control::Bytes> list_connections(const Work& item) {
        control::Reader reader(item.frame.payload);
        const auto version = reader.u32();
        const auto after = read_cursor(reader);
        if (!reader.done() || version != connections_version || !valid_cursor(after)) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopicConnections});
        }

        TopicConnectionsPage page;
        // One worker owns the registry. Stop at the first extra row; a single
        // topic with thousands of subscriptions still fits bounded wire pages.
        auto append = [&](const std::string& topic, std::uint64_t schema, ConnectionRole role,
                          std::uint64_t id, std::uint64_t owner) {
            ConnectionCursor key{topic, role, id};
            if (!cursor_before(after, key)) {
                return true;
            }
            if (page.connections.size() == connections_per_page) {
                page.next = page.connections.back().key;
                return false;
            }

            page.connections.push_back({std::move(key), schema, owner});
            return true;
        };
        for (auto it = topics.lower_bound(after.topic); it != topics.end(); ++it) {
            const auto& [name, topic] = *it;
            for (const auto& [id, ring] : topic.publishers) {
                if (!append(name, topic.schema, ConnectionRole::Publisher, id, ring->descriptor.owner)) {
                    return encode_connections(page);
                }
            }
            for (const auto& [id, subscriber] : topic.subscribers) {
                if (!append(name, topic.schema, ConnectionRole::Subscriber, id, subscriber.owner)) {
                    return encode_connections(page);
                }
            }
        }

        return encode_connections(page);
    }

    vinc::Result<control::Bytes> execute(const Work& item) {
        switch (item.frame.operation) {
        case ListTopicConnections:
            try {
                return list_connections(item);
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::ListTopicConnections});
            }
        case InspectTopics:
            return inspect_topics(item);
        case ListTopics:
            return list_topics(item);
        case Advertise:
            return advertise(item);
        case Subscribe:
            return subscribe(item);
        case EnableNotifications:
            return enable_notifications(item);
        case ClosePublisher:
        case Unsubscribe:
            return close_registration(item);
        default:
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Receive));
        }
    }

    // Serialize registration and cleanup work outside the ModuleManager supervisor.
    // pending_work includes the active item, allowing shutdown to wait for cleanup completion.
    void run() {
        for (;;) {
            Work item;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return stopping || !work.empty();
                });
                if (work.empty()) {
                    break;
                }
                item = std::move(work.front());
                work.pop_front();
            }

            struct Completed {
                Impl& self;

                ~Completed() {
                    std::lock_guard lock(self.mutex);
                    --self.pending_work;
                    self.cv.notify_all();
                }
            } completed{*this};

            try {
                if (item.exit) {
                    exited(item.owner);
                    continue;
                }

                auto [peer, added] = peers.try_emplace(item.owner, Peer{item.session, 0, item.notification_name});
                if (peer->second.session != item.session || item.frame.domain != message_bus_domain) {
                    item.session->close();
                    continue;
                }

                if (item.frame.kind == control::Kind::Request) {
                    if (!item.frame.request || item.frame.request <= peer->second.last_request) {
                        item.session->close();
                        continue;
                    }

                    peer->second.last_request = item.frame.request;
                    response(item, execute(item));
                } else if (item.frame.kind == control::Kind::Event && !item.frame.request &&
                           (item.frame.operation == ClosePublisher || item.frame.operation == Unsubscribe)) {
                    if (!execute(item)) {
                        item.session->close();
                    }
                } else {
                    item.session->close();
                }

                prune();
            } catch (...) {
                if (item.session) {
                    item.session->close();
                }
            }
        }

        topics.clear();
        peers.clear();
    }
};

BusManager::BusManager(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {
}

vinc::Result<std::unique_ptr<BusManager>> BusManager::create(Limits settings, std::shared_ptr<vinc::detail::ResourceNames> resources) {
    if (!settings.max_topics || !settings.publishers_per_topic ||
        settings.publishers_per_topic > maximum_attached_publishers || !settings.subscribers_per_topic ||
        settings.subscribers_per_topic > maximum_ring_buffer_readers ||
        settings.max_shared_bytes < minimum_bus_manager_shared_bytes) {
        return std::unexpected(vinc::Error{ErrorCode::InvalidArgument, ErrorOperation::Create});
    }

    try {
        auto impl = std::make_unique<Impl>(settings, std::move(resources));
        auto result = std::unique_ptr<BusManager>(new BusManager(std::move(impl)));
        result->impl_->start_worker();
        return result;
    } catch (const std::system_error& failure) {
        return std::unexpected(vinc::Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker,
                                           osal::native_error(failure.code())});
    } catch (const std::bad_alloc&) {
        return std::unexpected(vinc::Error{ErrorCode::ResourceExhausted, ErrorOperation::Create});
    }
}

BusManager::~BusManager() = default;

vinc::Result<void> BusManager::submit(std::uint64_t owner, std::string name,
                                      std::shared_ptr<control::Session> session, control::Frame frame, std::string notification_name) {
    // Reject old/unknown versions before queueing any registration or release work.
    if (frame.domain != message_bus_domain || !supported_operation(frame.operation)) {
        return std::unexpected(vinc::Error{ErrorCode::Protocol, ErrorOperation::Submit});
    }

    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
        return std::unexpected(vinc::Error{ErrorCode::Closed, ErrorOperation::Submit});
    }
    if (impl_->work.size() >= config::message_bus_manager_queue_size) {
        return std::unexpected(vinc::Error{ErrorCode::CapacityExceeded, ErrorOperation::Submit});
    }

    impl_->work.push_back({owner, std::move(name), std::move(session), std::move(frame), false, std::move(notification_name)});
    ++impl_->pending_work;
    impl_->cv.notify_all();
    return {};
}

void BusManager::exited(std::uint64_t owner) {
    std::lock_guard lock(impl_->mutex);
    // Cleanup is never discarded due to the application request queue limit.
    impl_->work.push_back({owner, {}, {}, {}, true});
    ++impl_->pending_work;
    impl_->cv.notify_all();
}

vinc::Result<void> BusManager::drain_until(vinc::Deadline deadline) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->cv.wait_until(lock, deadline, [&] {
            return impl_->pending_work == 0;
        })) {
        return std::unexpected(vinc::Error{ErrorCode::Timeout, ErrorOperation::Drain});
    }
    return {};
}
}
