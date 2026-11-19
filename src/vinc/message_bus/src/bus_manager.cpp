// Central Message Bus registry and shared-memory owner. One worker serializes topology changes
// and process-exit cleanup; payload copies never pass through this BusManager.

#include <vinc/time.hpp>
#include <vinc/message_bus/detail/topology_protocol.hpp>
#include <vinc/message_bus/detail/diagnostics_protocol.hpp>
#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/control/channel.hpp>
#include <algorithm>
#include <array>
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

Result<std::uint64_t> ring_bytes(PublisherDefaults settings, const Limits& limits) {
    if (!settings.slots || settings.slots > maximum_ring_buffer_slots ||
        settings.max_message_bytes < minimum_message_bytes ||
        settings.max_message_bytes > SIZE_MAX - payload_alignment_mask ||
        settings.max_message_bytes > limits.max_shared_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Advertise});
    }

    const auto slots = (std::max)(minimum_ring_buffer_slots, settings.slots);
    const auto stride = RingBuffer::slot_stride(settings.max_message_bytes);
    if (stride > limits.max_shared_bytes / slots) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Advertise});
    }

    const auto data_bytes = slots * stride;
    const auto metadata_bytes = RingBuffer::control_bytes(slots, limits.subscribers_per_topic);
    if (data_bytes > UINT64_MAX - metadata_bytes) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Advertise});
    }

    return data_bytes + metadata_bytes;
}
}

Result<void> validate_publisher_defaults(PublisherDefaults settings, const Limits& limits) {
    auto needed = ring_bytes(settings, limits);
    if (!needed) {
        return std::unexpected(needed.error());
    }

    if (limits.max_shared_bytes < sizeof(SharedWord) ||
        *needed > limits.max_shared_bytes - sizeof(SharedWord)) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Advertise});
    }

    return {};
}

struct BusManager::Impl {
    using Records = std::vector<control::Bytes>;
    struct SavedTopic {
        Records records;
        std::size_t charge;
    };

    // A keyset cursor into the registry. Only a changed, unread Topic needs
    // its pre-change wire image. No snapshot pins shared memory or reader slots.
    struct Snapshot {
        explicit Snapshot(std::uint64_t boundary) : cut(boundary) {}
        std::uint64_t cut;
        std::string topic;
        std::size_t next_record = 0;
        bool finished_topic = false;
        bool final_page = false;
        bool superseded = false;
        std::map<std::string, SavedTopic> saved;
        std::size_t saved_bytes = 0;
    };

    struct Peer {
        std::shared_ptr<control::Session> session;
        std::uint64_t last_request = 0;
        std::string notification_name;
        Peer(std::shared_ptr<control::Session> channel, std::string wakeup, std::uint64_t owner)
            : session(std::move(channel)), notification_name(std::move(wakeup)), cursor(owner) {}

        TopologySendCursor cursor;
        std::uint64_t revision = 0;
        std::uint64_t target = 0;
        TopologyMode mode = TopologyMode::Delta;
        bool snapshot_needed = false;
        std::size_t history_bytes = 0;
        std::deque<control::Bytes> history;
        std::deque<control::Bytes> pages;
        std::optional<Snapshot> snapshot;
    };

    struct Sub {
        std::uint64_t id;
        std::uint64_t owner;
        std::uint32_t index;
        bool notifications = false;
        // Only pre-existing Publishers with a nonzero NewOnly boundary need
        // storage. Later Publishers start at zero, even during snapshot repair.
        std::map<std::uint64_t, std::uint64_t> starting_floors;

        std::uint64_t floor(std::uint64_t publisher) const {
            const auto found = starting_floors.find(publisher);
            return found == starting_floors.end() ? 0 : found->second;
        }
    };

    // A topic shares its schema and count, but each Publisher owns a separate ring buffer.
    // Adding another Publisher does not relocate memory already mapped by existing clients.
    struct Topic {
        std::string name;
        std::uint64_t schema = 0;
        std::shared_ptr<NamedCounter> counter;
        std::map<std::uint64_t, std::shared_ptr<RingBuffer>> publishers;
        std::map<std::uint64_t, Sub> subscribers;
        // Ordered membership groups keep Attach/Detach work local to this Topic.
        std::map<std::uint64_t, std::set<std::uint64_t>> local_readers;
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
    PublisherDefaultsSource defaults;
    TopicLimitSource topic_limit;
    std::uint64_t next_id = 1;
    std::uint64_t allocated = 0;
    std::size_t history_bytes = 0;
    std::set<std::uint64_t> snapshot_hosts;
    std::set<std::uint64_t> ready_hosts;
    std::set<std::uint64_t> blocked_hosts;
    Deadline retry_at = Deadline::max();
    std::map<std::uint64_t, TopologyAck> queued_receipts; // protected by mutex
    std::map<std::uint64_t, TopologyStatus> status; // protected by mutex
    std::map<std::string, Topic> topics;
    std::map<std::uint64_t, Peer> peers;
    // Only the worker touches topics/peers/allocated. This mutex protects the work
    // queue, stopping flag and pending_work; it is released before OS or Socket work.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Work> work;
    bool stopping = false;
    std::size_t pending_work = 0;
    std::optional<RegistrationCleanupFailure> last_cleanup; // protected by mutex
    std::thread worker;

    explicit Impl(Limits settings, std::shared_ptr<vinc::detail::ResourceNames> names,
                  PublisherDefaultsSource source, TopicLimitSource limit_source)
        : resources(std::move(names)), limits(settings), defaults(std::move(source)), topic_limit(std::move(limit_source)) {
    }

    void record_rollback(Error primary, RegistrationCleanupFailure&& failure) {
        if (failure.counter || failure.control || failure.payload) {
            failure.primary = primary;
            std::lock_guard lock(mutex);
            last_cleanup = std::move(failure);
        }
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

        if (item.frame.operation == ListTopicConnections || item.frame.operation == CheckSource) {
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

        auto maximum = limits.max_topics;
        if (topic_limit) {
            auto current = topic_limit();
            if (!current) {
                return std::unexpected(io_resource(current.error(), ErrorOperation::CreateTopic));
            }

            maximum = *current;
        }

        const bool count_exhausted = maximum != 0 && topics.size() >= maximum;
        if (count_exhausted || allocated > limits.max_shared_bytes - sizeof(SharedWord)) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::CreateTopic));
        }

        RegistrationCleanupFailure cleanup;
        auto counter = NamedCounter::create(resources ? std::string{} : shared_name('t'), resources, cleanup.counter);
        if (!counter) {
            record_rollback(counter.error(), std::move(cleanup));
            return std::unexpected(counter.error());
        }

        Topic topic_state;
        topic_state.name = name;
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

    void publish_status(std::uint64_t owner, const Peer& peer, bool progressed) {
        std::lock_guard lock(mutex);
        auto& value = status[owner];
        value.applied = peer.cursor.applied();
        value.target = peer.revision;
        if (progressed || value.last_progress == Deadline{}) {
            value.last_progress = std::chrono::steady_clock::now();
        }
    }

    void clear_history(Peer& peer) {
        history_bytes -= peer.history_bytes;
        peer.history.clear();
        peer.history_bytes = 0;
    }

    void queue_change(std::uint64_t owner, std::uint32_t kind, const control::Bytes& body) {
        auto found = peers.find(owner);
        if (found == peers.end()) {
            return;
        }
        auto& peer = found->second;
        if (peer.revision == UINT64_MAX || body.size() + topology_record_header > topology_page_capacity) {
            peer.session->close();
            return;
        }
        ++peer.revision;
        const auto bytes = topology_record_header + body.size();
        if (!peer.snapshot_needed) {
            if (peer.history.size() >= config::message_bus_topology_history_records ||
                bytes > config::message_bus_topology_history_bytes - peer.history_bytes ||
                bytes > config::message_bus_topology_total_history_bytes - history_bytes) {
                clear_history(peer);
                peer.snapshot_needed = true;
            } else {
                control::Bytes record;
                append_topology_record(record, kind, body);
                peer.history.push_back(std::move(record));
                peer.history_bytes += bytes;
                history_bytes += bytes;
            }
        }
        if (peer.pages.empty()) {
            ready_hosts.insert(owner);
        }
        publish_status(owner, peer, false);
    }

    // One Publisher descriptor serves all of this Host's reader leases.
    template<class Emit>
    static void source_records(const Descriptor& descriptor, std::span<const TopologyReader> readers, Emit emit) {
        while (!readers.empty()) {
            const auto count = (std::min)(topology_group_readers, readers.size());
            auto encoded = encode_source_group(descriptor, readers.first(count));
            emit(std::move(encoded));
            readers = readers.subspan(count);
        }
    }

    // At most one Topic is encoded in temporary workspace. Its size is bounded
    // by admitted readers, Publishers and the control route/descriptor limits.
    Result<Records> snapshot_records(const Topic& topic, std::uint64_t owner, std::uint64_t cut) const {
        Records result;
        const auto add = [&](auto kind, const control::Bytes& body) {
            append_topology_record(result.emplace_back(), kind, body);
        };
        std::vector<TopologyReader> local;
        const auto group = topic.local_readers.find(owner);
        if (group != topic.local_readers.end()) {
            for (auto id : group->second) {
                if (id <= cut) {
                    local.push_back({id, topic.subscribers.at(id).index, 0});
                }
            }
        }
        for (const auto& [id, ring] : topic.publishers) {
            if (id > cut) {
                break;
            }
            for (auto& reader : local) {
                reader.floor = topic.subscribers.at(reader.subscription).floor(id);
            }
            std::optional<Error> failure;
            source_records(ring->descriptor, local, [&](auto encoded) {
                if (encoded) {
                    add(Attach, *encoded);
                } else {
                    failure = encoded.error();
                }
            });
            if (failure) {
                return std::unexpected(*failure);
            }
            if (ring->descriptor.owner == owner) {
                control::Bytes body;
                control::put_u64(body, id);
                const auto routes = notification_routes(topic);
                body.insert(body.end(), routes.begin(), routes.end());
                add(NotificationRoutesChanged, body);
            }
        }
        return result;
    }

    void discard_saved(Snapshot& snapshot) {
        history_bytes -= snapshot.saved_bytes;
        snapshot.saved_bytes = 0;
        snapshot.saved.clear();
    }

    // Save the unread version before a removal or a route mutation. New endpoint
    // IDs are above the cut and need no undo. On budget pressure the outgoing
    // page remains valid; the next staged ACK permits a fresh transfer serial.
    void preserve_topic(const Topic& topic) {
        for (auto owner : snapshot_hosts) {
            auto& peer = peers.at(owner);
            auto& snapshot = *peer.snapshot;
            if (snapshot.superseded || snapshot.final_page || topic.name < snapshot.topic ||
                (topic.name == snapshot.topic && snapshot.finished_topic) || snapshot.saved.contains(topic.name)) {
                continue;
            }
            if (!topic.local_readers.contains(owner) && std::ranges::none_of(topic.publishers, [owner](const auto& entry) {
                    return entry.second->descriptor.owner == owner;
                })) {
                continue;
            }
            try {
                auto records = snapshot_records(topic, owner, snapshot.cut);
                if (!records) {
                    peer.session->close();
                    continue;
                }
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::checkpoint(testing::FaultPoint::SnapshotHistory);
#endif
                std::size_t charge = topic.name.size() + sizeof(SavedTopic);
                for (const auto& record : *records) {
                    charge += record.size() + sizeof(control::Bytes);
                }
                if (charge <= config::message_bus_topology_history_bytes - snapshot.saved_bytes &&
                    charge <= config::message_bus_topology_total_history_bytes - history_bytes) {
                    snapshot.saved.emplace(topic.name, SavedTopic{std::move(*records), charge});
                    history_bytes += charge;
                    snapshot.saved_bytes += charge;
                    continue;
                }
            } catch (const std::bad_alloc&) {
                // The current wire page already owns its bytes. Recovery needs
                // no extra undo record and cannot interfere with endpoint cleanup.
            }
            discard_saved(snapshot);
            snapshot.superseded = true;
        }
    }

    void next_snapshot_page(std::uint64_t owner, Peer& peer) {
        auto& snapshot = *peer.snapshot;
        auto& page = peer.pages.emplace_back();
        if (snapshot.topic.empty()) {
            control::Bytes cut;
            control::put_u64(cut, snapshot.cut);
            append_topology_record(page, snapshot_cut, cut);
        }
        for (;;) {
            auto live = snapshot.finished_topic ? topics.upper_bound(snapshot.topic) : topics.lower_bound(snapshot.topic);
            auto saved = snapshot.finished_topic ? snapshot.saved.upper_bound(snapshot.topic)
                                                 : snapshot.saved.lower_bound(snapshot.topic);
            if (live == topics.end() && saved == snapshot.saved.end()) {
                snapshot.final_page = true;
                return;
            }

            const bool use_saved = saved != snapshot.saved.end() && (live == topics.end() || saved->first <= live->first);
            snapshot.topic = use_saved ? saved->first : live->first;
            snapshot.finished_topic = false;
            Records scratch;
            if (!use_saved) {
                auto result = snapshot_records(live->second, owner, snapshot.cut);
                if (!result) {
                    peer.session->close();
                    return;
                }
                scratch = std::move(*result);
            }
            const auto& records = use_saved ? saved->second.records : scratch;
            while (snapshot.next_record < records.size()) {
                const auto& record = records[snapshot.next_record];
                if (record.size() > topology_page_capacity - page.size()) {
                    return;
                }
                page.insert(page.end(), record.begin(), record.end());
                ++snapshot.next_record;
            }
            if (use_saved) {
                snapshot.saved_bytes -= saved->second.charge;
                history_bytes -= saved->second.charge;
                snapshot.saved.erase(saved);
            }
            snapshot.next_record = 0;
            snapshot.finished_topic = true;
        }
    }

    void prepare_transfer(std::uint64_t owner, Peer& peer) {
        peer.target = peer.revision;
        if (peer.snapshot_needed) {
            peer.mode = TopologyMode::Snapshot;
            peer.snapshot.emplace(next_id - 1);
            snapshot_hosts.insert(owner);
            next_snapshot_page(owner, peer);
            peer.snapshot_needed = false;
        } else {
            peer.mode = TopologyMode::Delta;
            for (const auto& record : peer.history) {
                if (peer.pages.empty() || peer.pages.back().size() + record.size() > topology_page_capacity) {
                    peer.pages.emplace_back();
                }
                auto& page = peer.pages.back();
                page.insert(page.end(), record.begin(), record.end());
            }
        }
        clear_history(peer);
    }

    void send_page(std::uint64_t owner, Peer& peer) {
        if (peer.pages.empty()) {
            if (peer.snapshot) {
                next_snapshot_page(owner, peer);
            } else {
                if (!peer.snapshot_needed && peer.history.empty()) {
                    return;
                }
                prepare_transfer(owner, peer);
                if (!peer.cursor.start(peer.target)) {
                    peer.session->close();
                    return;
                }
            }
        }
        if (!peer.cursor.ready_to_send()) {
            return;
        }
        const bool final = peer.snapshot ? peer.snapshot->final_page : peer.pages.size() == 1;
        const auto base = peer.mode == TopologyMode::Snapshot ? 0 : peer.cursor.applied();
        auto encoded = encode_topology_page({peer.cursor.id(), base, peer.target, peer.mode, final, peer.pages.front()});
        if (!encoded) {
            peer.session->close();
            return;
        }
        Result<void> sent;
#ifdef VINC_MESSAGE_BUS_TESTING
        if (testing::reject_topology_admission()) {
            sent = std::unexpected(Error{control::ErrorCode::Backpressure, control::ErrorOperation::Send});
        } else
#endif
        {
            sent = peer.session->send({control::Kind::Event, message_bus_domain, SyncPage, 0, std::move(*encoded)});
        }
        if (sent) {
            peer.cursor.sent(final);
        } else if (sent.error().is(control::ErrorCode::Backpressure)) {
            blocked_hosts.insert(owner);
            if (retry_at == Deadline::max()) {
                retry_at = std::chrono::steady_clock::now() + config::message_bus_topology_send_retry;
            }
        } else {
            peer.session->close();
        }
    }

    void flush_pages() {
        if (std::chrono::steady_clock::now() >= retry_at) {
            ready_hosts.merge(blocked_hosts);
            blocked_hosts.clear();
            retry_at = Deadline::max();
        }
        for (const auto owner : ready_hosts) {
            auto found = peers.find(owner);
            if (found != peers.end()) {
                try {
                    send_page(owner, found->second);
                } catch (const std::bad_alloc&) {
                    found->second.session->close();
                }
            }
        }
        ready_hosts.clear();
#ifdef VINC_MESSAGE_BUS_TESTING
        // Sample only in the private test build, on the registry's owning worker.
        std::size_t prepared_bytes = 0;
        std::size_t saved_bytes = 0;
        for (const auto& [owner, peer] : peers) {
            for (const auto& page : peer.pages) {
                prepared_bytes += page.size();
            }
            if (peer.snapshot) {
                saved_bytes += peer.snapshot->saved_bytes;
            }
        }
        testing::topology_storage(history_bytes, prepared_bytes, saved_bytes);
#endif
    }

    void receipt(std::uint64_t owner, Peer& peer, const control::Frame& frame) {
        auto ack = decode_topology_ack(frame.payload);
        if (!ack) {
            peer.session->close();
            return;
        }
        auto action = peer.cursor.acknowledge(*ack);
        if (!action) {
            peer.session->close();
            return;
        }
        if (*action == AckAction::IgnoreOld) {
            return;
        }
        const bool replace = *action == AckAction::NextPage && peer.snapshot && peer.snapshot->superseded;
        if (replace) {
            peer.cursor.supersede_staged();
        }
        if (peer.snapshot && (*action == AckAction::Finished || *action == AckAction::RestartSnapshot || replace)) {
            discard_saved(*peer.snapshot);
            peer.snapshot.reset();
            snapshot_hosts.erase(owner);
        }
        if (*action == AckAction::RestartSnapshot || replace) {
            peer.pages.clear();
            clear_history(peer);
            peer.snapshot_needed = true;
        } else {
            peer.pages.pop_front();
        }
        publish_status(owner, peer, true);
        ready_hosts.insert(owner);
    }

    void notify_detached(std::uint64_t owner, std::uint64_t publisher, const Topic& topic) noexcept {
        try {
#ifdef VINC_MESSAGE_BUS_TESTING
            testing::checkpoint(testing::FaultPoint::DetachEncode);
#endif
            const auto& ids = topic.local_readers.at(owner);
            auto it = ids.begin();
            std::size_t remaining = ids.size();
            while (remaining) {
                const auto count = (std::min)(remaining, topology_group_readers);
                control::Bytes body;
                control::put_u64(body, publisher);
                control::put_u32(body, static_cast<std::uint32_t>(count));
                for (std::size_t index = 0; index < count; ++index) {
                    control::put_u64(body, *it++);
                }
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::checkpoint(testing::FaultPoint::DetachSend);
#endif
                queue_change(owner, Detach, body);
                remaining -= count;
            }
        } catch (...) {
            peers.at(owner).session->close();
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
        preserve_topic(topic_state);
        auto ring_buffer = it->second;
        const auto publisher = it->first;
        ring_buffer->retire();
        for (auto& [_, subscriber] : topic_state.subscribers) {
            collect_reader(topic_state, *ring_buffer, subscriber.index);
            subscriber.starting_floors.erase(publisher);
        }
        allocated -=
            RingBuffer::control_bytes(ring_buffer->descriptor.slots, ring_buffer->descriptor.readers) +
            ring_buffer->descriptor.slots * RingBuffer::slot_stride(ring_buffer->descriptor.bytes);
        topic_state.publishers.erase(it);
        for (const auto& [owner, _] : topic_state.local_readers) {
            notify_detached(owner, publisher, topic_state);
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

    // One Module generation has one wakeup endpoint, including when several
    // of its independent subscriptions enable callbacks for the same Topic.
    static bool has_notification_owner(const Topic& topic, std::uint64_t owner) {
        return std::ranges::any_of(topic.subscribers, [owner](const auto& entry) {
            return entry.second.owner == owner && entry.second.notifications;
        });
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
                queue_change(publisher->descriptor.owner, NotificationRoutesChanged, frame.payload);
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
                if (has_notification_owner(topic_state, item.owner) || peers.at(item.owner).notification_name.empty()) {
                    found->second.notifications = true;
                    return control::Bytes{};
                }
                const auto routes = notification_routes(topic_state, id);
                // Reserve the largest encoded Descriptor, so subscriber-first
                // registration also fits a later Advertise response. This is a
                // wire ceiling, separate from the configured reader capacity.
                constexpr auto descriptor_fixed_bytes = 5 * sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t);
                constexpr auto descriptor_text_bytes = 4 * (sizeof(std::uint32_t) + maximum_name_bytes);
                constexpr auto route_budget = control::max_payload - response_status_bytes -
                                              sizeof(std::uint32_t) - descriptor_fixed_bytes - descriptor_text_bytes;
                if (routes.size() > route_budget) {
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::EnableNotifications});
                }

                preserve_topic(topic_state);
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
        preserve_topic(topic_state);
        for (auto& [_, publisher] : topic_state.publishers) {
            collect_reader(topic_state, *publisher, it->second.index);
            publisher->reset_reader(it->second.index);
        }

        topic_state.used[it->second.index] = false;
        const auto owner = it->second.owner;
        const bool notifications = it->second.notifications;
        auto group = topic_state.local_readers.find(owner);
        group->second.erase(it->first);
        if (group->second.empty()) {
            topic_state.local_readers.erase(group);
        }
        topic_state.subscribers.erase(it);
        auto peer = peers.find(owner);
        if (notifications && peer != peers.end() && !peer->second.notification_name.empty() &&
            !has_notification_owner(topic_state, owner)) {
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

        if (auto peer = peers.find(owner); peer != peers.end()) {
            clear_history(peer->second);
            if (peer->second.snapshot) {
                discard_saved(*peer->second.snapshot);
                snapshot_hosts.erase(owner);
            }
            peers.erase(peer);
        }
        ready_hosts.erase(owner);
        blocked_hosts.erase(owner);
        {
            std::lock_guard lock(mutex);
            status.erase(owner);
        }
        prune();
    }

    // Run on the BusManager worker. Validate schema and aggregate memory limits before
    // creating resources, then notify subscriptions of the resulting topology change.
    vinc::Result<control::Bytes> advertise(const Work& item) {
        control::Reader reader(item.frame.payload);
        control::Bytes body;
        const auto name = reader.string();
        const auto schema = reader.u64();
        const auto present = reader.u32();
        PublisherOptions options;
        if (present & publisher_slots_present) {
            options.slots = reader.u32();
        }
        if (present & publisher_bytes_present) {
            options.max_message_bytes = reader.u64();
        }
        const bool valid_slots = !options.slots || (*options.slots != 0 && *options.slots <= maximum_ring_buffer_slots);
        const bool valid_bytes = !options.max_message_bytes ||
            (*options.max_message_bytes >= minimum_message_bytes &&
             *options.max_message_bytes <= SIZE_MAX - payload_alignment_mask &&
             *options.max_message_bytes <= limits.max_shared_bytes);
        if (!reader.done() || (present & ~publisher_options_mask) || !valid_slots || !valid_bytes ||
            item.name.size() > maximum_name_bytes || !valid_topic_separators(name)) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise));
        }

        // Preserve topic-identity errors while rejecting malformed identities
        // before the defaults source can read or repair any parameters.
        if (name.empty() || name.size() > maximum_name_bytes || name.find('\0') != std::string::npos || !schema) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::CreateTopic));
        }

        PublisherDefaults settings;
        if ((!options.slots || !options.max_message_bytes) && defaults) {
            auto current = defaults();
            if (!current) {
                return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::Advertise, current.error()));
            }

            settings = *current;
        }
        const auto requested = options.slots.value_or(settings.slots);
        const auto bytes = options.max_message_bytes.value_or(settings.max_message_bytes);
        auto ring_size = ring_bytes({requested, bytes}, limits);
        if (!ring_size) {
            return std::unexpected(ring_size.error());
        }

        const auto slots = (std::max)(minimum_ring_buffer_slots, requested);
        const auto needed = *ring_size;
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
        if (!resources) {
            descriptor.control = shared_name('c');
            descriptor.data = shared_name('d');
        }
        descriptor.counter = topic_state.counter->name;
        descriptor.publisher = item.name;
        RegistrationCleanupFailure cleanup;
        auto ring_buffer = RingBuffer::create(std::move(descriptor), resources, cleanup);
        if (!ring_buffer) {
            record_rollback(ring_buffer.error(), std::move(cleanup));
            return std::unexpected(ring_buffer.error());
        }

        const auto& published = (*ring_buffer)->descriptor;
        control::put_u32(body, requested);
        auto encoded = encode_descriptor(body, published);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto routes = notification_routes(topic_state);
        body.insert(body.end(), routes.begin(), routes.end());
        if (body.size() + response_status_bytes > control::max_payload) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Advertise});
        }

        (*ring_buffer)->requested_slots = requested;
        topic_state.publishers.emplace(published.id, *ring_buffer);
        allocated += needed;
        for (const auto& [owner, ids] : topic_state.local_readers) {
            try {
                std::vector<TopologyReader> local;
                local.reserve(ids.size());
                for (auto id : ids) {
                    local.push_back({id, topic_state.subscribers.at(id).index, 0});
                }
                source_records(published, local, [&](auto encoded) {
                    if (encoded) {
                        queue_change(owner, Attach, *encoded);
                    } else {
                        peers.at(owner).session->close();
                    }
                });
            } catch (const std::bad_alloc&) {
                peers.at(owner).session->close();
            }
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
        Sub subscription{id, item.owner, index, false, {}};
        control::put_u64(body, id);
        control::put_u32(body, static_cast<std::uint32_t>(topic_state.publishers.size()));
        for (const auto& [_, publisher] : topic_state.publishers) {
            const auto floor = start ? shared_load(publisher->header().head) : 0;
            if (floor != 0) {
                subscription.starting_floors.emplace(publisher->descriptor.id, floor);
            }
            auto attachment = encode_attachment(body, {publisher->descriptor, index, subscription.floor(publisher->descriptor.id)});
            if (!attachment) {
                return std::unexpected(attachment.error());
            }
        }

        if (body.size() + response_status_bytes > control::max_payload) {
            return std::unexpected(
                vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Subscribe));
        }

        auto [group, _] = topic_state.local_readers.try_emplace(item.owner);
        try {
            group->second.insert(id);
            topic_state.subscribers.emplace(id, std::move(subscription));
        } catch (...) {
            group->second.erase(id);
            if (group->second.empty()) {
                topic_state.local_readers.erase(group);
            }
            throw;
        }
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

    // The worker owns the registry for the entire validation and application.
    // Topic pointers stay valid until prune(), after all IDs have been retired.
    Topic* owned_topic(const EndpointRelease& release, std::uint64_t owner) {
        for (auto& [_, topic] : topics) {
            if (release.operation == Unsubscribe) {
                auto sub = topic.subscribers.find(release.id);
                if (sub != topic.subscribers.end() && sub->second.owner == owner) {
                    return &topic;
                }
            } else {
                auto pub = topic.publishers.find(release.id);
                if (pub != topic.publishers.end() && pub->second->descriptor.owner == owner) {
                    return &topic;
                }
            }
        }
        return nullptr;
    }

    Result<control::Bytes> release_batch(const Work& item) {
        const Error invalid{ErrorCode::Protocol, ErrorOperation::CloseRegistration};
        control::Reader reader(item.frame.payload);
        const auto count = reader.u32();
        if (!count || count > maximum_releases_per_batch) {
            return std::unexpected(invalid);
        }

        std::array<EndpointRelease, maximum_releases_per_batch> releases{};
        std::array<Topic*, maximum_releases_per_batch> owners{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto operation = reader.u32();
            const auto id = reader.u64();
            if ((operation != Unsubscribe && operation != ClosePublisher) || !id) {
                return std::unexpected(invalid);
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (releases[prior].id == id) {
                    return std::unexpected(invalid);
                }
            }

            releases[index] = {static_cast<std::uint16_t>(operation), id};
            owners[index] = owned_topic(releases[index], item.owner);
            if (!owners[index]) {
                return std::unexpected(invalid);
            }
        }
        if (!reader.done()) {
            return std::unexpected(invalid);
        }

        for (std::size_t index = 0; index < count; ++index) {
            auto& topic = *owners[index];
            const auto& release = releases[index];
            if (release.operation == Unsubscribe) {
                remove_subscriber(topic, topic.subscribers.find(release.id));
            } else {
                remove_publisher(topic, topic.publishers.find(release.id));
            }
        }

        // The reply promises capacity is reusable, including the last endpoint's Topic.
        prune();
        return control::Bytes{};
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

    Result<control::Bytes> check_source(const Work& item) const {
        control::Reader reader(item.frame.payload);
        const auto name = reader.string();
        const auto subscription = reader.u64();
        const auto publisher = reader.u64();
        if (!reader.done() || name.empty() || name.size() > maximum_name_bytes || !subscription || !publisher) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }

        bool present = false;
        const auto topic = topics.find(name);
        if (topic != topics.end()) {
            const auto sub = topic->second.subscribers.find(subscription);
            present = sub != topic->second.subscribers.end() && sub->second.owner == item.owner &&
                      topic->second.publishers.contains(publisher);
        }

        control::Bytes body;
        control::put_u32(body, present ? 1 : 0);
        return body;
    }

    vinc::Result<control::Bytes> execute(const Work& item) {
        switch (item.frame.operation) {
        case CheckSource:
            return check_source(item);
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
        case ReleaseBatch:
            return release_batch(item);
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
                const auto has_work = [&] { return stopping || !work.empty(); };
                if (retry_at == Deadline::max()) {
                    cv.wait(lock, has_work);
                } else {
                    cv.wait_until(lock, retry_at, has_work);
                }
                if (work.empty()) {
#ifdef VINC_MESSAGE_BUS_TESTING
                    testing::topology_worker_wake();
#endif
                    if (stopping) {
                        break;
                    }
                    lock.unlock();
                    flush_pages();
                    continue;
                }
                item = std::move(work.front());
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::topology_worker_wake();
#endif
                work.pop_front();
                if (item.frame.operation == SyncAck) {
                    queued_receipts.erase(item.owner);
                }
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
                    flush_pages();
                    continue;
                }

                if (item.frame.operation == SyncAck && !peers.contains(item.owner)) {
                    continue;
                }
                auto [peer, added] = peers.try_emplace(item.owner, item.session, item.notification_name, item.owner);
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
                } else if (item.frame.kind == control::Kind::Event && !item.frame.request && item.frame.operation == SyncAck) {
                    receipt(item.owner, peer->second, item.frame);
                } else if (item.frame.kind == control::Kind::Event && !item.frame.request &&
                           (item.frame.operation == ClosePublisher || item.frame.operation == Unsubscribe)) {
                    if (!execute(item)) {
                        item.session->close();
                    }
                } else {
                    item.session->close();
                }

                if (item.frame.operation != ReleaseBatch && item.frame.operation != CheckSource && item.frame.operation != SyncAck) {
                    prune();
                }
                flush_pages();
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

vinc::Result<std::unique_ptr<BusManager>> BusManager::create(Limits settings,
    std::shared_ptr<vinc::detail::ResourceNames> resources, PublisherDefaultsSource defaults, TopicLimitSource topic_limit) {
    if (!settings.publishers_per_topic ||
        settings.publishers_per_topic > maximum_attached_publishers || !settings.subscribers_per_topic ||
        settings.subscribers_per_topic > maximum_ring_buffer_readers ||
        settings.max_shared_bytes < minimum_bus_manager_shared_bytes) {
        return std::unexpected(vinc::Error{ErrorCode::InvalidArgument, ErrorOperation::Create});
    }

    try {
        auto impl = std::make_unique<Impl>(settings, std::move(resources), std::move(defaults), std::move(topic_limit));
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

std::optional<RegistrationCleanupFailure> BusManager::last_registration_cleanup_failure() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last_cleanup;
}

std::optional<TopologyStatus> BusManager::topology_status(std::uint64_t owner) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->status.find(owner);
    if (found == impl_->status.end()) {
        return std::nullopt;
    }
    return found->second;
}

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
    if (frame.operation == SyncAck) {
        auto ack = decode_topology_ack(frame.payload);
        if (frame.kind != control::Kind::Event || frame.request || !ack) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Submit});
        }
        if (ack->id.generation != owner) {
            return {};
        }
        auto [slot, inserted] = impl_->queued_receipts.try_emplace(owner, *ack);
        if (!inserted) {
            if (slot->second.id == ack->id && slot->second.receipt == ack->receipt) {
                return {};
            }
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Submit});
        }
    } else if (impl_->work.size() >= config::message_bus_manager_queue_size) {
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
