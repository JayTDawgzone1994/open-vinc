// Central Message Bus registry and shared-memory owner. One worker serializes topology changes
// and process-exit cleanup; payload copies never pass through this BusManager.

#include <vinc/time.hpp>
#include <vinc/message_bus/detail/protocol.hpp>
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
struct BusManager::Impl {
    struct Peer {
        std::shared_ptr<control::Session> session;
        std::uint64_t last_request = 0;
    };

    struct Sub {
        std::uint64_t id;
        std::uint64_t owner;
        std::uint32_t index;
    };

    // A topic shares its schema and count, but each Publisher owns a separate ring buffer.
    // Adding another Publisher does not relocate memory already mapped by existing clients.
    struct Topic {
        std::uint64_t schema = 0;
        std::shared_ptr<NamedCounter> counter;
        std::map<std::uint64_t, std::shared_ptr<RingBuffer>> publishers;
        std::map<std::uint64_t, Sub> subscribers;
        std::vector<bool> used;
    };

    struct Work {
        std::uint64_t owner;
        std::string name;
        std::shared_ptr<control::Session> session;
        control::Frame frame;
        bool exit = false;
    };

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

    explicit Impl(Limits settings) : limits(settings) {
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

        auto counter = NamedCounter::create(shared_name('t'));
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

    void remove_publisher(Topic& topic_state,
                          std::map<std::uint64_t, std::shared_ptr<RingBuffer>>::iterator it) {
        auto ring_buffer = it->second;
        const auto publisher = it->first;
        ring_buffer->retire();
        allocated -=
            RingBuffer::control_bytes(ring_buffer->descriptor.slots, ring_buffer->descriptor.readers) +
            ring_buffer->descriptor.slots * RingBuffer::slot_stride(ring_buffer->descriptor.bytes);
        topic_state.publishers.erase(it);
        for (const auto& [_, subscriber] : topic_state.subscribers) {
            notify_detached(subscriber, publisher);
        }
    }

    // Called only after a quiescent unsubscribe or confirmed owner-process exit.
    // Clearing a live reader lease based only on a timeout would permit unsafe overwrite.
    void remove_subscriber(Topic& topic_state, std::map<std::uint64_t, Sub>::iterator it) {
        for (auto& [_, publisher] : topic_state.publishers) {
            publisher->clear_reader(it->second.index);
        }

        topic_state.used[it->second.index] = false;
        topic_state.subscribers.erase(it);
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
        descriptor.control = shared_name('c');
        descriptor.data = shared_name('d');
        descriptor.counter = topic_state.counter->name;
        descriptor.publisher = item.name;
        auto ring_buffer = RingBuffer::create(descriptor);
        if (!ring_buffer) {
            return std::unexpected(ring_buffer.error());
        }

        auto encoded = encode_descriptor(body, descriptor);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
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

    vinc::Result<control::Bytes> execute(const Work& item) {
        switch (item.frame.operation) {
        case Advertise:
            return advertise(item);
        case Subscribe:
            return subscribe(item);
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

                auto [peer, added] = peers.try_emplace(item.owner, Peer{item.session});
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

vinc::Result<std::unique_ptr<BusManager>> BusManager::create(Limits settings) {
    if (!settings.max_topics || !settings.publishers_per_topic ||
        settings.publishers_per_topic > maximum_attached_publishers || !settings.subscribers_per_topic ||
        settings.subscribers_per_topic > maximum_ring_buffer_readers ||
        settings.max_shared_bytes < minimum_bus_manager_shared_bytes) {
        return std::unexpected(vinc::Error{ErrorCode::InvalidArgument, ErrorOperation::Create});
    }

    try {
        auto impl = std::make_unique<Impl>(settings);
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
                                      std::shared_ptr<control::Session> session, control::Frame frame) {
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

    impl_->work.push_back({owner, std::move(name), std::move(session), std::move(frame), false});
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
