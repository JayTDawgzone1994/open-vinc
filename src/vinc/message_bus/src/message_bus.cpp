#include <vinc/message_bus/detail/dispatch.hpp>
// Module-side Message Bus client: register through the control session, then read/write mapped rings.
// User message objects stay local; shared write loans are explicitly sealed and committed.

#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/time.hpp>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#ifdef VINC_MESSAGE_BUS_TESTING
#include "bus_test_hooks.hpp"
#endif

namespace vinc::message_bus::detail {
namespace {
Result<std::uint64_t> publication_timestamp() noexcept {
#ifdef VINC_MESSAGE_BUS_TESTING
    return testing::publication_timestamp();
#else
    return vinc::unix_nanoseconds();
#endif
}
}

template <class F> auto guarded(ErrorOperation operation, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, operation));

    } catch (const std::exception&) {
        return std::unexpected(vinc::make_error(ErrorCode::InvalidData, operation));
    }
}

// Share the existing module control session. The module receive dispatcher fills
// responses/topology while caller threads wait; no second reader competes for Socket frames.
struct Transport {
    struct Pending {
        std::uint16_t operation;
        std::optional<control::Frame> response;
    };

    ControlTransport connection;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t next_request = 1;
    bool closed = false;
    std::map<std::uint64_t, Pending> pending;
    std::map<std::uint64_t, std::map<std::uint64_t, Attachment>> topology;

    explicit Transport(ControlTransport transport)
        : connection(std::move(transport)) {
    }

    // Caller holds mutex; closing the session makes an unknown registration result
    // terminal instead of replaying a potentially successful remote mutation.
    void fail_locked() {
        closed = true;
        connection.close();
        changed.notify_all();
    }

    vinc::Result<control::Bytes> request(std::uint16_t operation, control::Bytes bytes) {
        std::unique_lock lock(mutex);
        if (closed) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Request));
        }

        if (pending.size() >= config::message_bus_pending_requests || next_request == UINT64_MAX) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Request));
        }

        const auto id = next_request++;
        pending.emplace(id, Pending{operation, {}});
        // This guard is destroyed before lock. An exception after admission may
        // leave the remote outcome unknown, so close rather than permit replay.
        struct PendingCleanup {
            Transport& transport;
            std::uint64_t id;
            bool settled = false;

            ~PendingCleanup() {
                transport.pending.erase(id);
                if (!settled) {
                    transport.fail_locked();
                }
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::pending_remaining(transport.pending.size());
#endif
            }
        } cleanup{*this, id};
#ifdef VINC_MESSAGE_BUS_TESTING
        testing::checkpoint(testing::FaultPoint::RequestSend);
#endif
        auto sent = connection.send({control::Kind::Request, message_bus_domain, operation, id, std::move(bytes)});
        if (!sent) {
            fail_locked();
            return std::unexpected(vinc::wrap_error(ErrorCode::Closed, ErrorOperation::Request, sent.error()));
        }

        const bool response_received =
            changed.wait_until(lock, std::chrono::steady_clock::now() + config::message_bus_request_timeout, [&] {
                return closed || pending.at(id).response.has_value();
            });
        if (!response_received || closed) {
            fail_locked();
            return std::unexpected(vinc::make_error(
                response_received ? ErrorCode::Closed : ErrorCode::Timeout, ErrorOperation::Request));
        }

        auto frame = std::move(*pending.at(id).response);
        auto response = decode_response(frame.payload);
        if (!response) {
            fail_locked();
            return std::unexpected(response.error());
        }
        if (response->failure) {
            // A remote Bus error already belongs to this component; do not wrap it again.
            cleanup.settled = true;
            return std::unexpected(*response->failure);
        }

#ifdef VINC_MESSAGE_BUS_TESTING
        testing::checkpoint(testing::FaultPoint::ResponseCopy);
#endif
        control::Bytes body(response->body.begin(), response->body.end());
        cleanup.settled = true;
        return body;
    }

    void release(std::uint16_t operation, std::uint64_t id) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (operation == Unsubscribe) {
                topology.erase(id);
            }

            if (closed) {
                return;
            }

            control::Frame frame{control::Kind::Event, message_bus_domain, operation};
            control::put_u64(frame.payload, id);
            if (!connection.send(std::move(frame))) {
                fail_locked();
            }
        } catch (...) {
            connection.close();
        }
    }

    bool receive(control::Frame frame) {
        std::lock_guard lock(mutex);
        if (closed || frame.domain != message_bus_domain || !supported_operation(frame.operation)) {
            return false;
        }

        control::Reader reader(frame.payload);
        if (frame.kind == control::Kind::Response) {
            auto found = pending.find(frame.request);
            if (found == pending.end() || found->second.operation != frame.operation ||
                found->second.response) {
                fail_locked();
                return false;
            }

            auto response = decode_response(frame.payload);
            if (!response) {
                fail_locked();
                return false;
            }

            if (!response->failure && frame.operation == Subscribe) {
                control::Reader subscription_reader(response->body);
                const auto id = subscription_reader.u64();
                const auto count = subscription_reader.u32();
                if (!id || count > detail::maximum_attached_publishers || topology.contains(id)) {
                    fail_locked();
                    return false;
                }

                std::map<std::uint64_t, Attachment> attachments;
                for (std::uint32_t i = 0; i < count; ++i) {
                    auto attachment = decode_attachment(subscription_reader);
                    if (!attachments.emplace(attachment.descriptor.id, std::move(attachment)).second) {
                        fail_locked();
                        return false;
                    }
                }

                if (!subscription_reader.done()) {
                    fail_locked();
                    return false;
                }
                // Install topology before waking request(): later Attach events must
                // already recognize this subscription even if its caller has not resumed.
                topology.emplace(id, std::move(attachments));
            }

            found->second.response = std::move(frame);
            changed.notify_all();
            return true;
        }

        if (frame.kind != control::Kind::Event || frame.request) {
            fail_locked();
            return false;
        }

        const auto id = reader.u64();
        auto target = topology.find(id);
        if (frame.operation == Attach) {
            auto attachment = decode_attachment(reader);
            if (!reader.done() || !attachment.descriptor.id ||
                attachment.reader >= attachment.descriptor.readers) {
                fail_locked();
                return false;
            }
            // A queued topology event can arrive after local unsubscribe.
            if (target != topology.end()) {
                if (target->second.size() >= maximum_attached_publishers ||
                    !target->second.emplace(attachment.descriptor.id, std::move(attachment)).second) {
                    fail_locked();
                    return false;
                }
            }
        } else if (frame.operation == Detach) {
            const auto publisher = reader.u64();
            if (!reader.done()) {
                fail_locked();
                return false;
            }

            if (target != topology.end()) {
                target->second.erase(publisher);
            }
        } else {
            fail_locked();
            return false;
        }

        return true;
    }
};

// A successful RPC is not yet an owned Publisher/Subscriber. Until handoff,
// release a validated identity, or close the session if the reply is untrusted.
// The enclosing Client keeps Transport alive; this guard itself never allocates.
struct RegistrationCleanup {
    Transport& transport;
    std::uint16_t operation;
    std::uint64_t id = 0;
    bool done = false;

    ~RegistrationCleanup() {
        if (done) {
            return;
        }

        if (id) {
            transport.release(operation, id);
        } else {
            std::lock_guard lock(transport.mutex);
            transport.fail_locked();
        }
    }
};

struct LoanCore {
    std::shared_ptr<RingBuffer> ring_buffer;
    Reservation reservation;
    message::detail::Binding binding;
    std::atomic<bool> active{true};
};

// The operation mutex prevents overlapping writer operations within this process.
// Shared atomics separately protect the cross-process reader/writer boundary.
struct PublisherCore {
    std::shared_ptr<Transport> transport;
    std::shared_ptr<RingBuffer> ring_buffer;
    std::shared_ptr<NamedCounter> counter;
    std::uint32_t requested = 0;
    std::mutex operation;
    std::atomic<bool> closed{false};
    std::shared_ptr<LoanCore> outstanding;
};

// The caller's registration guard owns remote rollback throughout attachment.
// A partially attached core retains its mappings until that guard has sent ClosePublisher.
vinc::Result<void> attach_publisher_resources(PublisherCore& core, const Descriptor& descriptor) {
    auto ring_buffer = RingBuffer::open(descriptor, true);
    if (!ring_buffer) {
        return std::unexpected(ring_buffer.error());
    }

    core.ring_buffer = std::move(*ring_buffer);
#ifdef VINC_MESSAGE_BUS_TESTING
    testing::checkpoint(testing::FaultPoint::CounterOpen);
#endif
    auto counter = NamedCounter::open(descriptor.counter);
    if (!counter) {
        return std::unexpected(counter.error());
    }

    core.counter = std::move(*counter);
    return {};
}

struct SubscriberCore {
    struct Source {
        Attachment attachment;
        std::shared_ptr<RingBuffer> ring_buffer;
        std::uint64_t cursor = 0;
        PublisherLoss losses;
    };

    std::shared_ptr<Transport> transport;
    std::uint64_t id = 0;
    std::uint64_t schema = 0;
    std::string topic;
    std::mutex operation;
    std::atomic<bool> closed{false};
    std::map<std::uint64_t, Source> sources;
    SubscriberStats losses;
    std::uint64_t max_message_bytes = 0;
};

class MessageAccess {
public:
    static void metadata(BaseMessage& message, const std::string& topic, const Descriptor& publisher,
                         const Metadata& metadata) {
        message._topic_name = topic;
        message._publisher_name = publisher.publisher;
        message._message_timestamp = metadata.timestamp;
        message._message_monotonic_timestamp = metadata.monotonic_timestamp;
        message._topic_count = metadata.count;
        message._publisher_instance = publisher.id;
    }
};

vinc::Result<LoanBinding> borrow(const std::shared_ptr<PublisherCore>& publisher,
                           const message::detail::Schema& schema) {
    return guarded(ErrorOperation::Borrow, [&]() -> vinc::Result<LoanBinding> {
        if (!publisher) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Borrow));
        }

        std::unique_lock lock(publisher->operation, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::unexpected(vinc::make_error(ErrorCode::OperationInProgress, ErrorOperation::Borrow));
        }

        if (publisher->closed.load()) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Borrow));
        }

        if (publisher->outstanding) {
            return std::unexpected(vinc::make_error(ErrorCode::OutstandingLoan, ErrorOperation::Borrow));
        }

        auto loan = std::make_shared<LoanCore>();
        loan->ring_buffer = publisher->ring_buffer;
        auto reservation = publisher->ring_buffer->reserve(publisher->closed);
        if (!reservation) {
            return std::unexpected(reservation.error());
        }

        loan->reservation = *reservation;
        auto binding = message::detail::FrameworkAccess::borrow_record(
            schema, reservation->bytes, publisher->ring_buffer, publisher->ring_buffer->descriptor.bytes);
        if (!binding) {
            publisher->ring_buffer->abandon(*reservation);
            return std::unexpected(storage_error(binding.error(), ErrorOperation::Borrow));
        }

        loan->binding = *binding;
        publisher->outstanding = loan;
        return LoanBinding{std::move(loan), std::move(*binding)};
    });
}

void abandon(const std::shared_ptr<LoanCore>& loan) noexcept {
    if (loan && loan->active.exchange(false)) {
        message::detail::release_message(loan->binding.state);
    }
    // Intentionally retain Publisher::outstanding. Dropping a loan is a misuse,
    // not an implicit cancellation that makes another borrow possible.
}

bool usable(const std::shared_ptr<LoanCore>& loan) noexcept {
    return loan && loan->active.load() && loan->ring_buffer->alive();
}

void construction_failed(const std::shared_ptr<PublisherCore>& publisher,
                         const std::shared_ptr<LoanCore>& loan) noexcept {
    if (!publisher || !loan) {
        return;
    }

    std::lock_guard lock(publisher->operation);
    if (publisher->outstanding == loan) {
        abandon(loan);
        publisher->ring_buffer->abandon(loan->reservation);
        publisher->outstanding.reset();
    }
}

// Only a matching outstanding loan can be committed. Seal revokes writable field
// access, then timestamp/count are assigned immediately before ring buffer publication.
vinc::Result<void> commit(const std::shared_ptr<PublisherCore>& publisher, const std::shared_ptr<LoanCore>& loan,
                    message::detail::Binding binding) {
    return guarded(ErrorOperation::Commit, [&]() -> vinc::Result<void> {
        if (!publisher) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Commit));
        }

        std::unique_lock lock(publisher->operation, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::unexpected(vinc::make_error(ErrorCode::OperationInProgress, ErrorOperation::Commit));
        }

        if (publisher->closed.load()) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Commit));
        }

        if (!loan || publisher->outstanding != loan || !usable(loan) ||
            binding.state != loan->binding.state || binding.handle != loan->binding.handle) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidLoan, ErrorOperation::Commit));
        }

        auto image = message::detail::seal_binding(binding);
        if (!image) {
            return std::unexpected(storage_error(image.error(), ErrorOperation::Commit));
        }

        const auto timestamp = publication_timestamp();
        if (!timestamp) {
            loan->active.store(false);
            return std::unexpected(wrap_error(ErrorCode::TimestampUnavailable, ErrorOperation::Commit,
                                              timestamp.error()));
        }

        const auto monotonic_timestamp = osal::monotonic_nanoseconds();
        auto count = publisher->counter->next();
        if (!count) {
            loan->active.store(false);
            return std::unexpected(count.error());
        }

        auto published =
            publisher->ring_buffer->publish(loan->reservation, image->bytes().size(), *timestamp,
                                            monotonic_timestamp, *count);
        loan->active.store(false);
        if (published) {
            publisher->outstanding.reset();
        }

        return published;
    });
}

// Copied publication has no user-visible loan: reserve, render into shared storage,
// then publish. Rollback abandons an unpublished reservation when an intermediate step fails.
vinc::Result<void> push(const std::shared_ptr<PublisherCore>& publisher, message::detail::Binding binding) {
    return guarded(ErrorOperation::Push, [&]() -> vinc::Result<void> {
        if (!publisher) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Push));
        }

        std::unique_lock lock(publisher->operation, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::unexpected(vinc::make_error(ErrorCode::OperationInProgress, ErrorOperation::Push));
        }

        if (publisher->closed.load()) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Push));
        }

        if (publisher->outstanding) {
            return std::unexpected(vinc::make_error(ErrorCode::OutstandingLoan, ErrorOperation::Push));
        }

        auto usage = message::detail::usage(binding.state);
        if (!usage) {
            return std::unexpected(storage_error(usage.error(), ErrorOperation::Push));
        }

        if (usage->compact_bytes > publisher->ring_buffer->descriptor.bytes) {
            return std::unexpected(
                vinc::make_error(ErrorCode::CapacityExceeded, ErrorOperation::Push));
        }

        auto reservation = publisher->ring_buffer->reserve(publisher->closed);
        if (!reservation) {
            return std::unexpected(reservation.error());
        }
        struct Rollback {
            std::shared_ptr<RingBuffer> ring_buffer;
            Reservation reservation;
            bool done = false;
            ~Rollback() {
                if (!done) {
                    ring_buffer->abandon(reservation);
                }
            }
        } rollback{publisher->ring_buffer, *reservation};
        auto image = message::detail::publish_binding(binding, reservation->bytes, publisher->ring_buffer);
        if (!image) {
            return std::unexpected(storage_error(image.error(), ErrorOperation::Push));
        }

        const auto timestamp = publication_timestamp();
        if (!timestamp) {
            return std::unexpected(wrap_error(ErrorCode::TimestampUnavailable, ErrorOperation::Push,
                                              timestamp.error()));
        }

        const auto monotonic_timestamp = osal::monotonic_nanoseconds();
        auto count = publisher->counter->next();
        if (!count) {
            return std::unexpected(count.error());
        }

        auto published =
            publisher->ring_buffer->publish(*reservation, image->bytes().size(), *timestamp,
                                            monotonic_timestamp, *count);
        rollback.done = bool(published);
        return published;
    });
}

void close(const std::shared_ptr<PublisherCore>& publisher) noexcept {
    if (!publisher || publisher->closed.exchange(true)) {
        return;
    }

    publisher->ring_buffer->retire(); // wakes a reserve operation waiting on a live reader
    std::lock_guard lock(publisher->operation);
    abandon(publisher->outstanding);
    if (publisher->outstanding) {
        publisher->outstanding->ring_buffer.reset();
    }

    publisher->outstanding.reset();
    publisher->transport->release(ClosePublisher, publisher->ring_buffer->descriptor.id);
}

void close(const std::shared_ptr<SubscriberCore>& subscriber) noexcept {
    if (!subscriber || subscriber->closed.exchange(true)) {
        return;
    }

    std::lock_guard lock(subscriber->operation); // read operation's guard is released before unsubscribe
    subscriber->sources.clear();
    subscriber->transport->release(Unsubscribe, subscriber->id);
}

PublisherStats stats(const std::shared_ptr<PublisherCore>& publisher) {
    return publisher ? publisher->ring_buffer->stats(publisher->requested) : PublisherStats{};
}

// Caller holds SubscriberCore::operation. Snapshot topology under the transport
// mutex, then release it before opening OS mappings so receive/heartbeat can progress.
vinc::Result<void> refresh(SubscriberCore& subscriber) {
    std::map<std::uint64_t, Attachment> topology;
    {
        std::lock_guard lock(subscriber.transport->mutex);
        if (subscriber.transport->closed) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Refresh));
        }

        auto subscription = subscriber.transport->topology.find(subscriber.id);
        if (subscription == subscriber.transport->topology.end()) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Refresh));
        }
        topology = subscription->second;
    }

    for (auto it = subscriber.sources.begin(); it != subscriber.sources.end();) {
        if (!topology.contains(it->first)) {
            it = subscriber.sources.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& [id, attachment] : topology) {
        if (subscriber.sources.contains(id)) {
            continue;
        }

        if (attachment.descriptor.schema != subscriber.schema ||
            attachment.reader >= attachment.descriptor.readers) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Refresh));
        }

        auto ring_buffer = RingBuffer::open(attachment.descriptor, false);
        if (!ring_buffer) {
            // Retirement can race a previously received Attach event. If the
            // detach has arrived, ignore it; otherwise surface the real OS error.
            std::lock_guard lock(subscriber.transport->mutex);
            auto subscription = subscriber.transport->topology.find(subscriber.id);
            if (subscription == subscriber.transport->topology.end() || !subscription->second.contains(id) ||
                ring_buffer.error().is(ErrorCode::Closed)) {
                continue;
            }

            return std::unexpected(ring_buffer.error());
        }

        subscriber.sources.emplace(
            id, SubscriberCore::Source{attachment, std::move(*ring_buffer), attachment.floor});
    }

    return {};
}

// Merge one candidate per Publisher. Protect only the selected payload during memcpy;
// validation and C++ facade construction happen after releasing the shared read lease.
vinc::Result<std::unique_ptr<BaseMessage>> read_impl(const std::shared_ptr<SubscriberCore>& subscriber, bool latest,
                                          const message::detail::Schema& schema, Factory factory, const ReadBatch* batch) {
    const auto operation = latest ? ErrorOperation::GetLast : ErrorOperation::Next;
    return guarded(operation, [&]() -> vinc::Result<std::unique_ptr<BaseMessage>> {
        if (!subscriber) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, operation));
        }

        std::unique_lock lock(subscriber->operation, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::unexpected(vinc::make_error(ErrorCode::OperationInProgress, operation));
        }

        auto updated = refresh(*subscriber);
        if (!updated) {
            return std::unexpected(updated.error());
        }

        for (;;) {
            if (subscriber->closed.load()) {
                return std::unexpected(vinc::make_error(ErrorCode::Closed, operation));
            }
            SubscriberCore::Source* selected = nullptr;
            Metadata candidate;
            for (auto& [id, source] : subscriber->sources) {
                std::uint64_t upper = UINT64_MAX;
                if (batch) {
                    const auto boundary = std::find_if(batch->begin(), batch->end(), [id](const auto& item) {
                        return item.publisher == id;
                    });
                    if (boundary == batch->end() || source.cursor >= boundary->upper) {
                        continue;
                    }
                    upper = boundary->upper;
                }
                auto metadata = source.ring_buffer->candidate(source.cursor, latest, upper);
                if (batch && source.ring_buffer->alive()) {
                    // A loan evicts its previous slot before commit. Missing sequences up to
                    // this captured publication boundary are already irretrievable.
                    const auto skipped_to = metadata ? (std::min)(upper, metadata->sequence - 1) : upper;
                    if (skipped_to > source.cursor) {
                        ++source.losses.overwrite_events;
                        source.losses.lost_messages += skipped_to - source.cursor;
                        ++subscriber->losses.overwrite_events;
                        subscriber->losses.lost_messages += skipped_to - source.cursor;
                        source.cursor = skipped_to;
                    }
                }
                if (!metadata || metadata->sequence > upper) {
                    continue;
                }

                if (!selected || preferred(*metadata, source.attachment.descriptor.rank, candidate,
                                           selected->attachment.descriptor.rank, latest)) {
                    selected = &source;
                    candidate = *metadata;
                }
            }

            if (!selected) {
                const auto code = latest ? ErrorCode::NoPublishedData : ErrorCode::NoNewData;
                return std::unexpected(vinc::make_error(code, operation));
            }
            if (subscriber->max_message_bytes && candidate.size > subscriber->max_message_bytes) {
                if (latest) {
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, operation});
                }
                selected->cursor = candidate.sequence;
                ++subscriber->losses.oversized_messages;
                continue;
            }
            // Allocation and zero-initialization are outside the read protection.
            std::vector<std::byte> bytes(static_cast<std::size_t>(candidate.size));
            auto& ring_buffer = *selected->ring_buffer;
            const auto reader = selected->attachment.reader;
            // The writer may have recycled the slot since selection/allocation. Revalidate
            // after announcing the lease; retry rather than copying a torn or unrelated message.
            if (!ring_buffer.protect(candidate, reader)) {
                continue;
            }

            std::memcpy(bytes.data(), ring_buffer.payload(candidate).data(), bytes.size());
            ring_buffer.unprotect(reader); // Only the bounded payload copy held the lease.
            auto binding = message::detail::adopt_record(std::move(bytes), schema);
            if (!binding) {
                return std::unexpected(storage_error(binding.error(), operation));
            }

            auto value = factory(std::move(*binding));
            MessageAccess::metadata(*value, subscriber->topic, selected->attachment.descriptor, candidate);
            // Advance only after a complete local message exists. get_last() intentionally
            // leaves all next() cursors unchanged, including when it returns the same message again.
            if (!latest) {
                selected->cursor = candidate.sequence;
            }

            return value;
        }
    });
}

std::vector<Attachment> inspect_sources(const std::shared_ptr<SubscriberCore>& subscriber) {
    std::vector<Attachment> result;
    std::lock_guard lock(subscriber->transport->mutex);
    auto sources = subscriber->transport->topology.find(subscriber->id);
    if (sources != subscriber->transport->topology.end()) {
        for (auto& [id, attachment] : sources->second) {
            result.push_back(attachment);
        }
    }
    return result;
}

bool closed(const std::shared_ptr<SubscriberCore>& subscriber) noexcept {
    return !subscriber || subscriber->closed.load();
}

SubscriberStats stats(const std::shared_ptr<SubscriberCore>& subscriber) {
    if (!subscriber) {
        return {};
    }
    std::lock_guard lock(subscriber->operation);
    auto snapshot = subscriber->losses;
    snapshot.sources.reserve(subscriber->sources.size());
    for (auto& [id, source] : subscriber->sources) {
        auto loss = source.losses;
        loss.publisher = id;
        snapshot.sources.push_back(loss);
    }
    return snapshot;
}

Result<ReadBatch> capture(const std::shared_ptr<SubscriberCore>& subscriber) {
    return guarded(ErrorOperation::Next, [&]() -> Result<ReadBatch> {
        if (closed(subscriber)) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Next});
        }
        std::unique_lock lock(subscriber->operation, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::unexpected(Error{ErrorCode::OperationInProgress, ErrorOperation::Next});
        }
        auto updated = refresh(*subscriber);
        if (!updated) {
            return std::unexpected(updated.error());
        }
        ReadBatch batch;
        batch.reserve(subscriber->sources.size());
        for (auto& [id, source] : subscriber->sources) {
            batch.push_back({id, shared_load(source.ring_buffer->header().head)});
        }
        return batch;
    });
}

Result<std::unique_ptr<BaseMessage>> read_batch(const std::shared_ptr<SubscriberCore>& subscriber,
    const ReadBatch& batch, const message::detail::Schema& schema, Factory factory) {
    return read_impl(subscriber, false, schema, factory, &batch);
}

Result<std::unique_ptr<BaseMessage>> read(const std::shared_ptr<SubscriberCore>& subscriber,
    bool latest, const message::detail::Schema& schema, Factory factory) {
    return read_impl(subscriber, latest, schema, factory, nullptr);
}

}

namespace vinc::message_bus {
struct Client::Impl {
    std::shared_ptr<detail::Transport> transport;
};

Client::Client(std::shared_ptr<Impl> implementation) : impl_(std::move(implementation)) {
}

Client::~Client() = default;

vinc::Result<std::shared_ptr<detail::PublisherCore>> Client::advertise_raw(std::string topic, std::uint64_t schema,
                                                                     PublisherOptions options) {
    return detail::guarded(ErrorOperation::Advertise, [&]() -> vinc::Result<std::shared_ptr<detail::PublisherCore>> {
        if (!detail::valid_topic_separators(topic)) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise));
        }

        // Allocate local ownership before the remote mutation so failures can clean up.
        auto core = std::make_shared<detail::PublisherCore>();
        core->transport = impl_->transport;
        core->requested = options.slots;
        control::Bytes payload;
        auto encoded = control::put_string(payload, topic);
        if (!encoded) {
            return std::unexpected(
                vinc::wrap_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise, encoded.error()));
        }
        control::put_u64(payload, schema);
        control::put_u32(payload, options.slots);
        control::put_u64(payload, options.max_message_bytes);
        auto result = impl_->transport->request(detail::Advertise, std::move(payload));
        if (!result) {
            return std::unexpected(result.error());
        }

        detail::RegistrationCleanup cleanup{*impl_->transport, detail::ClosePublisher};
        control::Reader reader(*result);
#ifdef VINC_MESSAGE_BUS_TESTING
        detail::testing::checkpoint(detail::testing::FaultPoint::DescriptorDecode);
#endif
        auto descriptor = detail::decode_descriptor(reader);
        if (!reader.done() || !descriptor.id || descriptor.schema != schema) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Advertise));
        }
        cleanup.id = descriptor.id;
#ifdef VINC_MESSAGE_BUS_TESTING
        detail::testing::checkpoint(detail::testing::FaultPoint::PublisherAttach);
#endif
        auto attached = detail::attach_publisher_resources(*core, descriptor);
        if (!attached) {
            if (attached.error().is(ErrorCode::Protocol)) {
                // Invalid dimensions or mapped layout make the success reply
                // untrustworthy, just like malformed descriptor bytes.
                cleanup.id = 0;
            }

            return std::unexpected(attached.error());
        }

        cleanup.done = true;
        return core;
    });
}

Result<TopicPage> Client::list_topics(std::string after) {
    return detail::guarded(ErrorOperation::ListTopics, [&]() -> Result<TopicPage> {
        if (after.size() > detail::maximum_name_bytes) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::ListTopics});
        }
        control::Bytes payload;
        auto encoded = control::put_string(payload, after);
        if (!encoded) {
            return std::unexpected(wrap_error(ErrorCode::InvalidArgument, ErrorOperation::ListTopics, encoded.error()));
        }
        auto response = impl_->transport->request(detail::ListTopics, std::move(payload));
        if (!response) {
            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::ListTopics, response.error()));
        }
        control::Reader reader(*response);
        const auto count = reader.u32();
        if (count > detail::topics_per_page) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopics});
        }
        TopicPage result;
        std::string previous = after;
        for (std::uint32_t index = 0; index < count; ++index) {
            TopicInfo topic{reader.string(), reader.u64(), reader.u32()};
            if (topic.name <= previous || topic.name.size() > detail::maximum_name_bytes ||
                !topic.schema || !topic.publishers || topic.publishers > detail::maximum_attached_publishers) {
                return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopics});
            }
            previous = topic.name;
            result.topics.push_back(std::move(topic));
        }
        result.next = reader.string();
        if (!reader.done() || (!result.next.empty() && (count == 0 || result.next != previous))) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListTopics});
        }
        return result;
    });
}

vinc::Result<std::shared_ptr<detail::SubscriberCore>> Client::subscribe_raw(std::string topic, std::uint64_t schema,
                                                                      SubscriberOptions options) {
    return detail::guarded(ErrorOperation::Subscribe, [&]() -> vinc::Result<std::shared_ptr<detail::SubscriberCore>> {
        if (!detail::valid_topic_separators(topic)) {
            return std::unexpected(vinc::make_error(ErrorCode::InvalidArgument, ErrorOperation::Subscribe));
        }

        auto core = std::make_shared<detail::SubscriberCore>();
        core->transport = impl_->transport;
        core->schema = schema;
        core->topic = topic;
        core->max_message_bytes = options.max_message_bytes;
        control::Bytes payload;
        auto encoded = control::put_string(payload, topic);
        if (!encoded) {
            return std::unexpected(
                vinc::wrap_error(ErrorCode::InvalidArgument, ErrorOperation::Subscribe, encoded.error()));
        }
        control::put_u64(payload, schema);
        control::put_u32(payload, static_cast<std::uint32_t>(options.start));
        auto result = impl_->transport->request(detail::Subscribe, std::move(payload));
        if (!result) {
            return std::unexpected(result.error());
        }

        detail::RegistrationCleanup cleanup{*impl_->transport, detail::Unsubscribe};
        control::Reader reader(*result);
        core->id = reader.u64();
        const auto count = reader.u32();
        if (!core->id || count > detail::maximum_attached_publishers) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Subscribe));
        }

        // receive() already validated this complete reply before installing its
        // topology. The ID is now trustworthy even if local decoding allocates.
        cleanup.id = core->id;
        for (std::uint32_t i = 0; i < count; ++i) {
#ifdef VINC_MESSAGE_BUS_TESTING
            detail::testing::checkpoint(detail::testing::FaultPoint::AttachmentDecode);
#endif
            (void)detail::decode_attachment(reader);
        }

        if (!reader.done()) {
            cleanup.id = 0;
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Subscribe));
        }

        cleanup.done = true;
        return core;
    });
}

vinc::Result<std::shared_ptr<Client>>
detail::RuntimeAccess::create(ControlTransport connection) {
    if (!connection.send || !connection.close) {
        return std::unexpected(vinc::Error{ErrorCode::InvalidArgument, ErrorOperation::AttachRuntime});
    }
    try {
        auto implementation = std::make_shared<Client::Impl>();
        implementation->transport = std::make_shared<Transport>(std::move(connection));
        return std::shared_ptr<Client>(new Client(std::move(implementation)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(vinc::Error{ErrorCode::ResourceExhausted, ErrorOperation::AttachRuntime});
    }
}

vinc::Result<void> detail::RuntimeAccess::receive(Client& client, control::Frame frame) {
    try {
        if (client.impl_->transport->receive(std::move(frame))) {
            return {};
        }
        shutdown(client);
        return std::unexpected(vinc::Error{ErrorCode::Protocol, ErrorOperation::Receive});
    } catch (const std::bad_alloc&) {
        shutdown(client);
        return std::unexpected(vinc::Error{ErrorCode::ResourceExhausted, ErrorOperation::Receive});
    } catch (...) {
        shutdown(client);
        return std::unexpected(vinc::Error{ErrorCode::Io, ErrorOperation::Receive});
    }
}

void detail::RuntimeAccess::shutdown(Client& client) noexcept {
    auto& transport = *client.impl_->transport;
    std::lock_guard lock(transport.mutex);
    transport.fail_locked();
}

std::vector<detail::Attachment> detail::RuntimeAccess::inspect_attachments(Client& client) {
    auto& transport = *client.impl_->transport;
    std::lock_guard lock(transport.mutex);
    std::vector<Attachment> result;
    for (const auto& [_, sources] : transport.topology) {
        for (const auto& [id, source] : sources) {
            result.push_back(source);
        }
    }

    return result;
}
}
