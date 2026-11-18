#include <vinc/message_bus/detail/dispatch.hpp>
// Module-side Message Bus client: register through the control session, then read/write mapped rings.
// User message objects stay local; shared write loans are explicitly sealed and committed.

#include <vinc/message_bus/detail/topology_protocol.hpp>
#include <vinc/message_bus/detail/diagnostics_protocol.hpp>
#include <vinc/message_bus/detail/connections_protocol.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/time.hpp>
#include <condition_variable>
#include <list>
#include <cstring>
#include <map>
#include <mutex>
#ifdef VINC_MESSAGE_BUS_TESTING
#include "bus_test_hooks.hpp"
#endif
#ifdef VINC_SHUTDOWN_TESTING
#include "shutdown_test_hooks.hpp"
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
// Decode before opening anything. Native names come only from the authenticated
// central, but malformed fields still must not reach the OS namespace.
Result<std::vector<NotificationTarget>> decode_notification_targets(control::Reader& reader) {
    const auto count = reader.u32();
    if (count > maximum_ring_buffer_readers) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    std::vector<NotificationTarget> result;
    std::uint64_t previous = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        NotificationTarget target{reader.u64(), reader.string()};
        const auto valid_character = [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '_' || character == '-';
        };
        if (target.owner <= previous || target.name.empty() || target.name.size() > 24 ||
            !std::all_of(target.name.begin(), target.name.end(), valid_character)) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }
        previous = target.owner;
        result.push_back(std::move(target));
    }
    if (!reader.done()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    return result;
}

std::shared_ptr<const NotificationDestinations> prepare_notification_targets(NotificationRoutes& routes, const std::vector<NotificationTarget>& targets,
                                  const std::shared_ptr<NotificationTotals>& totals) {
    auto previous = routes.snapshot();
    auto next = std::make_shared<NotificationDestinations>();
    next->reserve(targets.size());
    const auto now = std::chrono::steady_clock::now();
    for (const auto& target : targets) {
        std::shared_ptr<NotificationDestination> destination;
        if (previous) {
            for (const auto& existing : *previous) {
                if (existing->target().owner == target.owner && existing->target().name == target.name) {
                    destination = existing;
                    break;
                }
            }
        }
        if (!destination) {
            destination = std::make_shared<NotificationDestination>(target, totals);
            destination->reopen(now);
        }
        next->push_back(std::move(destination));
    }
    return next;
}

void install_notification_targets(NotificationRoutes& routes, const std::vector<NotificationTarget>& targets,
                                  const std::shared_ptr<NotificationTotals>& totals) {
    routes.replace(prepare_notification_targets(routes, targets, totals));
    // Cover a publication racing route installation, including reused targets.
    routes.notify();
}

// One node is allocated before registration. Splicing it on retirement cannot allocate.
using ReleaseNode = std::list<EndpointRelease>;

// Shared only while a subscriber waits for the Host dispatcher to verify a
// missing native name. Normal reads and successfully mapped sources allocate none.
struct SourceCheck {
    std::uint64_t subscription;
    std::uint64_t publisher;
    std::string topic;
    Error first_failure;
    std::atomic<bool> confirmed_present{false};

    SourceCheck(std::uint64_t sub, std::uint64_t pub, const std::string& name, Error failure)
        : subscription(sub), publisher(pub), topic(name), first_failure(failure) {}
};

struct Transport {
    struct Pending {
        std::uint16_t operation;
        std::optional<control::Frame> response;
    };

    ControlTransport connection;
    std::shared_ptr<NotificationTotals> notification_totals = std::make_shared<NotificationTotals>();
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t next_request = 1;
    std::uint64_t retired_query = 0;
    bool closed = false;
    std::optional<Error> receive_failure;
    std::map<std::uint64_t, Pending> pending;
    std::map<std::uint64_t, std::map<std::uint64_t, Attachment>> topology;
    std::map<std::uint64_t, std::shared_ptr<NotificationRoutes>> notification_routes;

    std::optional<TopologyReceiveCursor> cursor;
    std::optional<TopologyAck> receipt;
    std::atomic<bool> receipt_pending{false};
    // All staged changes are private to the dispatcher until the final page.
    struct Update {
        std::uint32_t kind;
        std::uint64_t id = 0;
        SourceGroup source{};
        std::vector<std::uint64_t> subscriptions;
        std::vector<NotificationTarget> routes;
    };
    std::vector<Update> staged;

    ReleaseNode queued_releases;
    std::uint64_t released = 0;
    std::uint64_t confirmed = 0;
    std::uint64_t batch_request = 0;
    std::uint64_t batch_end = 0;
    Deadline batch_deadline{};
    std::atomic<bool> releases_pending{false};
    std::list<std::shared_ptr<SourceCheck>> source_checks;
    std::shared_ptr<SourceCheck> checking;
    std::uint64_t check_request = 0;
    std::uint64_t last_check_request = 0;
    Deadline next_check_attempt{};
    std::atomic<bool> checks_pending{false};

    // Caller holds mutex. Dropping the queue entry prevents an old reply
    // from resurrecting a closed subscription. At most one reply is outstanding.
    void forget_checks(std::uint64_t subscription, std::uint64_t publisher = 0) {
        if (!checks_pending.load()) {
            return;
        }
        source_checks.remove_if([&](const auto& check) {
            return check->subscription == subscription && (!publisher || check->publisher == publisher);
        });
        if (checking && checking->subscription == subscription && (!publisher || checking->publisher == publisher)) {
            checking.reset();
            check_request = 0;
            next_check_attempt = {};
        }
        checks_pending.store(!source_checks.empty());
    }

    explicit Transport(ControlTransport transport)
        : connection(std::move(transport)) {
        if (connection.generation) {
            cursor.emplace(connection.generation);
        }
    }

    // Caller holds mutex; closing the session makes an unknown registration result
    // terminal instead of replaying a potentially successful remote mutation.
#ifdef VINC_SHUTDOWN_TESTING
    void fail_locked(std::source_location location = std::source_location::current()) {
        if (!closed) {
            shutdown_test::first_bus_close(location);
        }
#else
    void fail_locked() {
#endif
        closed = true;
        connection.close();
        changed.notify_all();
    }

    // Snapshot once: a concurrent close cannot extend an older caller's fence.
    Result<void> await_release(std::unique_lock<std::mutex>& lock, std::uint64_t ticket, Deadline deadline) {
        if (ticket <= confirmed) {
            return {};
        }

#ifdef VINC_MESSAGE_BUS_TESTING
        if (testing::before_release_wait) {
            testing::before_release_wait();
        }
#endif
        if (!changed.wait_until(lock, deadline, [&] { return closed || confirmed >= ticket; })) {
            return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::CloseRegistration});
        }

        if (confirmed < ticket) {
            return std::unexpected(receive_failure.value_or(Error{ErrorCode::Closed, ErrorOperation::CloseRegistration}));
        }

#ifdef VINC_MESSAGE_BUS_TESTING
        if (testing::after_release_wait) {
            testing::after_release_wait(deadline);
        }
#endif
        return {};
    }

    vinc::Result<control::Bytes> request(std::uint16_t operation, control::Bytes bytes,
                                       Deadline deadline = (Deadline::max)()) {
        std::unique_lock lock(mutex);
        const bool read_only = operation == ListTopicConnections;
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::Request});
        }

        if (closed) {
            return std::unexpected(vinc::make_error(ErrorCode::Closed, ErrorOperation::Request));
        }

        deadline = (std::min)(deadline, std::chrono::steady_clock::now() + config::message_bus_request_timeout);
        auto prior_releases = await_release(lock, released, deadline);
        if (!prior_releases) {
            return std::unexpected(prior_releases.error());
        }

        if (closed) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Request});
        }

        // The release ACK can wake this caller before its deadline but it may resume later.
        // Reject here while the new request still has no ID or remote side effects.
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::Request});
        }

        if (pending.size() >= config::message_bus_pending_requests || next_request == UINT64_MAX) {
            return std::unexpected(vinc::make_error(ErrorCode::ResourceExhausted, ErrorOperation::Request));
        }

        const auto id = next_request++;
        pending.emplace(id, Pending{operation, {}});
        // This guard is destroyed before lock. An exception after admission may
        // leave a mutation outcome unknown. Only read-only queries can retire
        // their caller without closing the session.
        struct PendingCleanup {
            Transport& transport;
            std::uint64_t id;
            bool read_only;
            bool settled = false;

            ~PendingCleanup() {
                if (read_only) {
                    // Read replies have no mutation outcome to recover. One watermark
                    // retires old replies without leaking pending capacity after timeout.
                    transport.retired_query = (std::max)(transport.retired_query, id);
                }
                transport.pending.erase(id);
                if (!settled && !read_only) {
                    transport.fail_locked();
                }
#ifdef VINC_MESSAGE_BUS_TESTING
                testing::pending_remaining(transport.pending.size());
#endif
            }
        } cleanup{*this, id, read_only};
#ifdef VINC_MESSAGE_BUS_TESTING
        testing::checkpoint(testing::FaultPoint::RequestSend);
#endif
        auto sent = connection.send({control::Kind::Request, message_bus_domain, operation, id, std::move(bytes)});
        if (!sent) {
            if (read_only && !sent.error().is(control::ErrorCode::Closed)) {
                // Nothing was admitted to the transport, so no late reply is possible.
                cleanup.settled = true;
                return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::Request, sent.error()));
            }

            fail_locked();
            return std::unexpected(vinc::wrap_error(ErrorCode::Closed, ErrorOperation::Request, sent.error()));
        }

        const bool response_received =
            changed.wait_until(lock, deadline, [&] {
                return closed || pending.at(id).response.has_value();
            });
        if (!response_received || closed) {
            if (receive_failure) {
                return std::unexpected(*receive_failure);
            }
            if (!read_only || closed) {
                fail_locked();
            }
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

    std::uint64_t release(ReleaseNode& node, std::uint16_t operation, std::uint64_t id) noexcept {
        std::lock_guard lock(mutex);
        if (operation == Unsubscribe) {
            topology.erase(id);
            forget_checks(id);
        } else {
            notification_routes.erase(id);
        }

        node.front() = {operation, id};
        queued_releases.splice(queued_releases.end(), node);
        releases_pending.store(true);
        return ++released;
    }

    // Caller holds mutex. Never wait for a reply on the sole receiving thread.
    Result<void> progress_locked(Deadline now) {
        if (closed || !releases_pending.load()) {
            return {};
        }

        if (batch_deadline == Deadline{}) {
            batch_deadline = now + config::message_bus_request_timeout;
        }
        if (now >= batch_deadline) {
            return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::CloseRegistration});
        }
        if (batch_request != 0) {
            return {};
        }
        if (next_request == UINT64_MAX) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::CloseRegistration});
        }

        const auto count = (std::min<std::size_t>)(queued_releases.size(), maximum_releases_per_batch);
        control::Frame frame{control::Kind::Request, message_bus_domain, ReleaseBatch, next_request};
        control::put_u32(frame.payload, static_cast<std::uint32_t>(count));
        auto end = queued_releases.begin();
        for (std::size_t index = 0; index < count; ++index) {
            control::put_u32(frame.payload, end->operation);
            control::put_u64(frame.payload, end->id);
            ++end;
        }

        auto sent = connection.send(std::move(frame));
        if (!sent) {
            if (sent.error().is(control::ErrorCode::Backpressure)) {
                return {};
            }

            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::CloseRegistration, sent.error()));
        }

        batch_request = next_request++;
        batch_end = confirmed + count;
        queued_releases.erase(queued_releases.begin(), end);
        return {};
    }

    // The front stays in the queue until the reply arrives. A lost reply is
    // safe to retry: the operation only inspects the central registry.
    Result<void> progress_checks_locked(Deadline now) {
        if (source_checks.empty() || now < next_check_attempt) {
            return {};
        }
        if (checking) {
            source_checks.splice(source_checks.end(), source_checks, source_checks.begin());
            checking.reset();
            check_request = 0;
        }
        if (next_request == UINT64_MAX) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Refresh});
        }

        const auto& next = source_checks.front();
        control::Frame frame{control::Kind::Request, message_bus_domain, CheckSource, next_request, {}};
        auto name = control::put_string(frame.payload, next->topic);
        if (!name) {
            return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Refresh, name.error()));
        }
        control::put_u64(frame.payload, next->subscription);
        control::put_u64(frame.payload, next->publisher);
        auto sent = connection.send(std::move(frame));
        if (!sent) {
            if (sent.error().is(control::ErrorCode::Backpressure)) {
                next_check_attempt = now + config::message_bus_request_timeout;
                return {};
            }
            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::Refresh, sent.error()));
        }

        checking = next;
        check_request = next_request++;
        last_check_request = check_request;
        next_check_attempt = now + config::message_bus_request_timeout;
        return {};
    }

    Result<void> apply_check(const control::Frame& frame) {
        auto reply = decode_response(frame.payload);
        if (!reply || !frame.request || frame.request > last_check_request) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }
        control::Reader reader(reply->body);
        const auto present = reply->failure ? 0u : reader.u32();
        if (!reader.done() || present > 1) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }
        if (frame.request != check_request) {
            return {};
        }
        if (reply->failure && !reply->failure->is(ErrorCode::CapacityExceeded)) {
            return std::unexpected(*reply->failure);
        }

        if (reply->failure) {
            source_checks.splice(source_checks.end(), source_checks, source_checks.begin());
            next_check_attempt = std::chrono::steady_clock::now() + config::message_bus_request_timeout;
        } else {
            if (present) {
                checking->confirmed_present.store(true);
            } else {
                topology.at(checking->subscription).erase(checking->publisher);
            }
            source_checks.pop_front();
            next_check_attempt = {};
        }
        checking.reset();
        check_request = 0;
        checks_pending.store(!source_checks.empty());
        return {};
    }

    Result<void> send_receipt() {
        if (!receipt) {
            return {};
        }
        auto encoded = encode_topology_ack(*receipt);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto sent = connection.send({control::Kind::Event, message_bus_domain, SyncAck, 0, std::move(*encoded)});
        if (!sent) {
            if (sent.error().is(control::ErrorCode::Backpressure)) {
                return {};
            }
            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::Receive, sent.error()));
        }
        receipt.reset();
        receipt_pending.store(false);
        return {};
    }

    Result<void> progress() {
        if (!releases_pending.load() && !checks_pending.load() && !receipt_pending.load()) {
            return {};
        }

        std::lock_guard lock(mutex);
        auto result = guarded(ErrorOperation::CloseRegistration, [&] {
            const auto now = std::chrono::steady_clock::now();
            auto releases = progress_locked(now);
            if (!releases || closed) {
                return releases;
            }
            auto acknowledged = send_receipt();
            if (!acknowledged) {
                return acknowledged;
            }
            return progress_checks_locked(now);
        });
        if (!result) {
            receive_failure = result.error();
            fail_locked();
        }

        return result;
    }

    Result<void> wait_for_close(std::uint64_t ticket) {
        const auto deadline = std::chrono::steady_clock::now() + config::message_bus_request_timeout;
        auto started = progress();
        if (!started) {
            return started;
        }

        std::unique_lock lock(mutex);
        return await_release(lock, ticket, deadline);
    }

    Result<void> acknowledge(const control::Frame& frame) {
        auto response = decode_response(frame.payload);
        if (!batch_request || frame.request != batch_request || !response || !response->body.empty()) {
            receive_failure = Error{ErrorCode::Protocol, ErrorOperation::Receive};
        } else if (response->failure) {
            receive_failure = *response->failure;
        } else {
            confirmed = batch_end;
            batch_request = 0;
            batch_deadline = {};
            releases_pending.store(!queued_releases.empty());
            changed.notify_all();
            return {};
        }

        fail_locked();
        return std::unexpected(*receive_failure);
    }

    Result<void> receive(control::Frame frame) {
        std::unique_lock lock(mutex);
#ifdef VINC_SHUTDOWN_TESTING
        shutdown_test::bus_receive(closed, frame);
#endif
        if (closed) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
        }

        const auto protocol_failure = std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        if (frame.domain != message_bus_domain || !supported_operation(frame.operation)) {
            return protocol_failure;
        }

        control::Reader reader(frame.payload);
        if (frame.kind == control::Kind::Response) {
            if (frame.operation == CheckSource) {
                return apply_check(frame);
            }
            if (frame.operation == ReleaseBatch) {
                return acknowledge(frame);
            }

            auto found = pending.find(frame.request);
            if (found == pending.end() && frame.operation == ListTopicConnections &&
                frame.request != 0 && frame.request <= retired_query) {
                // Ignore an unclaimed old read-only reply, including duplicates. Its
                // envelope must still be valid; no topology or mutation state is applied.
                if (!decode_response(frame.payload)) {
                    fail_locked();
                    return protocol_failure;
                }
                return {};
            }
            if (found == pending.end() || found->second.operation != frame.operation ||
                found->second.response) {
                fail_locked();
                return protocol_failure;
            }

            auto response = decode_response(frame.payload);
            if (!response) {
                fail_locked();
                return protocol_failure;
            }

            if (!response->failure && frame.operation == Advertise) {
                control::Reader publication_reader(response->body);
                const auto requested_slots = publication_reader.u32();
                auto descriptor = decode_descriptor(publication_reader);
                auto targets = decode_notification_targets(publication_reader);
                if (!requested_slots || requested_slots > maximum_ring_buffer_slots ||
                    !publication_reader.done() || !descriptor.id || !targets ||
                    notification_routes.contains(descriptor.id)) {
                    receive_failure = Error{ErrorCode::Protocol, ErrorOperation::Receive};
                    fail_locked();
                    return protocol_failure;
                }
                auto routes = std::make_shared<NotificationRoutes>();
                notification_routes.emplace(descriptor.id, routes);
                lock.unlock();
                install_notification_targets(*routes, *targets, notification_totals);
                lock.lock();
                if (closed) {
                    return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
                }
            }

            if (!response->failure && frame.operation == Subscribe) {
                control::Reader subscription_reader(response->body);
                const auto id = subscription_reader.u64();
                const auto count = subscription_reader.u32();
                if (!id || count > detail::maximum_attached_publishers || topology.contains(id)) {
                    fail_locked();
                    return protocol_failure;
                }

                std::map<std::uint64_t, Attachment> attachments;
                for (std::uint32_t i = 0; i < count; ++i) {
                    auto attachment = decode_attachment(subscription_reader);
                    if (!attachments.emplace(attachment.descriptor.id, std::move(attachment)).second) {
                        fail_locked();
                        return protocol_failure;
                    }
                }

                if (!subscription_reader.done()) {
                    fail_locked();
                    return protocol_failure;
                }
                // Install topology before waking request(): later Attach events must
                // already recognize this subscription even if its caller has not resumed.
                topology.emplace(id, std::move(attachments));
            }

            found->second.response = std::move(frame);
            changed.notify_all();
            return {};
        }

        if (frame.kind != control::Kind::Event || frame.request) {
            fail_locked();
            return protocol_failure;
        }

        if (frame.operation != SyncPage) {
            return protocol_failure;
        }
        auto page = decode_topology_page(frame.payload);
        if (!page) {
            return std::unexpected(page.error());
        }
        return receive_page(*page, lock);
    }

    Result<void> stage_record(std::uint32_t kind, std::span<const std::byte> body, TopologyMode mode) {
        const Error invalid{ErrorCode::Protocol, ErrorOperation::Receive};
        Update update{};
        update.kind = kind;
        control::Reader reader(body);
        if (kind == Attach) {
            auto group = decode_source_group(body);
            if (!group) {
                return std::unexpected(group.error());
            }
            update.source = std::move(*group);
        } else if (kind == NotificationRoutesChanged) {
            update.id = reader.u64();
            auto targets = decode_notification_targets(reader);
            if (!update.id || !targets) {
                return std::unexpected(invalid);
            }
            update.routes = std::move(*targets);
        } else if (kind == Detach || kind == snapshot_cut) {
            update.id = reader.u64();
            if (kind == Detach) {
                const auto count = reader.u32();
                if (!count || count > topology_group_readers) {
                    return std::unexpected(invalid);
                }
                std::uint64_t previous = 0;
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto subscription = reader.u64();
                    if (subscription <= previous) {
                        return std::unexpected(invalid);
                    }
                    update.subscriptions.push_back(subscription);
                    previous = subscription;
                }
            }
            if (!update.id || !reader.done() ||
                (kind == snapshot_cut && (mode != TopologyMode::Snapshot || !staged.empty())) ||
                (kind == Detach && mode == TopologyMode::Snapshot)) {
                return std::unexpected(invalid);
            }
        } else {
            return std::unexpected(invalid);
        }
        staged.push_back(std::move(update));
        return {};
    }

    Result<void> commit_topology(TopologyMode mode, std::unique_lock<std::mutex>& lock) {
        const Error invalid{ErrorCode::Protocol, ErrorOperation::Receive};
        const bool snapshot = mode == TopologyMode::Snapshot;
        if (snapshot && (staged.empty() || staged.front().kind != snapshot_cut)) {
            return std::unexpected(invalid);
        }
        const auto cut = snapshot ? staged.front().id : UINT64_MAX;
        using Sources = std::map<std::uint64_t, Attachment>;
        std::map<std::uint64_t, Sources> changes;
        std::map<std::uint64_t, std::vector<NotificationTarget>> destinations;
        if (snapshot) {
            for (const auto& [id, _] : topology) {
                if (id <= cut) {
                    changes.try_emplace(id);
                }
            }
        }
        // Delta transactions copy only touched subscription rows. No mutation
        // becomes visible before every record has passed validation.
        auto editable = [&](auto sub) -> Sources& {
            auto [row, inserted] = changes.try_emplace(sub->first);
            if (inserted && !snapshot) {
                row->second = sub->second;
            }
            return row->second;
        };
        for (auto& update : staged) {
            if (update.kind == Attach) {
                const auto& publisher = update.source.publisher;
                if (publisher.id > cut) {
                    return std::unexpected(invalid);
                }
                for (const auto& reader : update.source.readers) {
                    if (reader.subscription > cut) {
                        return std::unexpected(invalid);
                    }
                    auto sub = topology.find(reader.subscription);
                    if (sub != topology.end()) {
                        auto& row = editable(sub);
                        if (row.size() >= maximum_attached_publishers ||
                            !row.emplace(publisher.id, Attachment{publisher, reader.index, reader.floor}).second) {
                            return std::unexpected(invalid);
                        }
                    }
                }
            } else if (update.kind == Detach) {
                for (auto id : update.subscriptions) {
                    auto sub = topology.find(id);
                    if (sub != topology.end()) {
                        editable(sub).erase(update.id);
                    }
                }
            } else if (update.kind == NotificationRoutesChanged) {
                if (update.id > cut || (snapshot && destinations.contains(update.id))) {
                    return std::unexpected(invalid);
                }
                destinations[update.id] = std::move(update.routes);
            }
        }

        using Prepared = std::pair<std::shared_ptr<NotificationRoutes>, std::shared_ptr<const NotificationDestinations>>;
        std::map<std::uint64_t, Prepared> prepared;
        for (auto& [id, targets] : destinations) {
            auto existing = notification_routes.find(id);
            if (existing != notification_routes.end()) {
                prepared.emplace(id, Prepared{existing->second, {}});
            }
        }
        lock.unlock();
        for (auto& [id, route] : prepared) {
            route.second = prepare_notification_targets(*route.first, destinations.at(id), notification_totals);
        }
        lock.lock();
        if (closed) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
        }

        for (auto& [id, row] : changes) {
            auto current = topology.find(id);
            if (current != topology.end()) {
                current->second.swap(row);
                for (const auto& [publisher, _] : row) {
                    if (!current->second.contains(publisher)) {
                        forget_checks(id, publisher);
                    }
                }
            }
        }
        for (auto& [id, route] : prepared) {
            if (notification_routes.contains(id)) {
                route.first->replace(std::move(route.second));
            } else {
                route.first.reset();
            }
        }
        lock.unlock();
        for (auto& [_, route] : prepared) {
            if (route.first) {
                route.first->notify();
            }
        }
        lock.lock();
        std::vector<Update>{}.swap(staged);
        return {};
    }

    Result<void> receive_page(const TopologyPage& page, std::unique_lock<std::mutex>& lock) {
        if (!cursor) {
            cursor.emplace(page.id.generation);
        }
        const auto action = cursor->classify(page);
        if (action == PageAction::IgnoreOld) {
            return {};
        }
        auto outcome = page.final ? TopologyReceipt::Applied : TopologyReceipt::Staged;
        if (action == PageAction::RequestSnapshot) {
            outcome = TopologyReceipt::Resync;
        } else if (action != PageAction::RepeatReceipt) {
            if (!page.id.index) {
                staged.clear();
            }
            auto decoded = visit_topology_records(page.records, [&](auto kind, auto body) {
                return stage_record(kind, body, page.mode);
            });
            if (!decoded) {
                return decoded;
            }
            if (action == PageAction::Commit) {
                auto committed = commit_topology(page.mode, lock);
                if (!committed) {
                    return committed;
                }
            }
            cursor->accept(page);
        }
        receipt = TopologyAck{page.id, outcome};
        receipt_pending.store(true);
        return send_receipt();
    }
};

// A successful RPC is not yet an owned Publisher/Subscriber. Until handoff,
// release a validated identity, or close the session if the reply is untrusted.
// The enclosing Client keeps Transport alive; this guard itself never allocates.
struct RegistrationCleanup {
    Transport& transport;
    ReleaseNode& node;
    std::uint16_t operation;
    std::uint64_t id = 0;
    bool done = false;

    ~RegistrationCleanup() {
        if (done) {
            return;
        }

        if (id) {
            transport.release(node, operation, id);
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
    ReleaseNode release_node{1};
    std::shared_ptr<Transport> transport;
    std::shared_ptr<RingBuffer> ring_buffer;
    std::shared_ptr<NamedCounter> counter;
    std::shared_ptr<NotificationRoutes> notifications;
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
    ReleaseNode release_node{1};
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
    std::map<std::uint64_t, std::shared_ptr<SourceCheck>> missing;
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
    auto result = guarded(ErrorOperation::Commit, [&]() -> vinc::Result<void> {
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
    if (result) {
        publisher->notifications->notify();
    }
    return result;
}

// Copied publication has no user-visible loan: reserve, render into shared storage,
// then publish. Rollback abandons an unpublished reservation when an intermediate step fails.
vinc::Result<void> push(const std::shared_ptr<PublisherCore>& publisher, message::detail::Binding binding) {
    auto result = guarded(ErrorOperation::Push, [&]() -> vinc::Result<void> {
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
    if (result) {
        publisher->notifications->notify();
    }
    return result;
}

std::uint64_t retire_ticket(const std::shared_ptr<PublisherCore>& publisher) noexcept {
    if (!publisher || publisher->closed.exchange(true)) {
        return 0;
    }

    publisher->ring_buffer->retire(); // wakes a reserve operation waiting on a live reader
    std::lock_guard lock(publisher->operation);
    abandon(publisher->outstanding);
    if (publisher->outstanding) {
        publisher->outstanding->ring_buffer.reset();
    }

    publisher->outstanding.reset();
    return publisher->transport->release(publisher->release_node, ClosePublisher, publisher->ring_buffer->descriptor.id);
}

void retire(const std::shared_ptr<PublisherCore>& publisher) noexcept {
    retire_ticket(publisher);
}

Result<void> close_confirmed(const std::shared_ptr<PublisherCore>& publisher) {
    const auto ticket = retire_ticket(publisher);
    if (!ticket) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::CloseRegistration});
    }

    return publisher->transport->wait_for_close(ticket);
}

std::uint64_t retire_ticket(const std::shared_ptr<SubscriberCore>& subscriber) noexcept {
    if (!subscriber || subscriber->closed.exchange(true)) {
        return 0;
    }

    std::lock_guard lock(subscriber->operation); // read operation's guard is released before unsubscribe
    subscriber->sources.clear();
    return subscriber->transport->release(subscriber->release_node, Unsubscribe, subscriber->id);
}

void retire(const std::shared_ptr<SubscriberCore>& subscriber) noexcept {
    retire_ticket(subscriber);
}

Result<void> close_confirmed(const std::shared_ptr<SubscriberCore>& subscriber) {
    const auto ticket = retire_ticket(subscriber);
    if (!ticket) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::CloseRegistration});
    }

    return subscriber->transport->wait_for_close(ticket);
}

PublisherStats stats(const std::shared_ptr<PublisherCore>& publisher) {
    return publisher ? publisher->ring_buffer->stats(publisher->requested) : PublisherStats{};
}

bool missing_shared_name(const Error& error) {
    if (!error.is_operation(ErrorOperation::OpenControl) && !error.is_operation(ErrorOperation::OpenPayload)) {
        return false;
    }
    const auto causes = error.causes();
    return !causes.empty() && causes.back().domain == ErrorDomain::Osal &&
           causes.back().code == static_cast<std::int32_t>(osal::ErrorCode::NotFound);
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

    std::erase_if(subscriber.missing, [&](const auto& entry) {
        return !topology.contains(entry.first);
    });
    for (auto& [id, attachment] : topology) {
        if (subscriber.sources.contains(id)) {
            continue;
        }

        if (attachment.descriptor.schema != subscriber.schema ||
            attachment.reader >= attachment.descriptor.readers) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Refresh));
        }

        const auto pending = subscriber.missing.find(id);
        if (pending != subscriber.missing.end() && !pending->second->confirmed_present.load()) {
            continue;
        }

        auto ring_buffer = RingBuffer::open(attachment.descriptor, false);
        if (!ring_buffer) {
            std::lock_guard lock(subscriber.transport->mutex);
            auto subscription = subscriber.transport->topology.find(subscriber.id);
            if (subscription == subscriber.transport->topology.end() || !subscription->second.contains(id) ||
                ring_buffer.error().is(ErrorCode::Closed)) {
                continue;
            }

            if (!missing_shared_name(ring_buffer.error())) {
                return std::unexpected(ring_buffer.error());
            }
            if (pending != subscriber.missing.end()) {
                return std::unexpected(pending->second->first_failure);
            }

            auto check = std::make_shared<SourceCheck>(subscriber.id, id, subscriber.topic, ring_buffer.error());
            // Insert locally first. An allocation failure in either container
            // must leave the next read able to schedule the check again.
            auto inserted = subscriber.missing.emplace(id, check).first;
            try {
                subscriber.transport->source_checks.push_back(std::move(check));
            } catch (const std::bad_alloc&) {
                subscriber.missing.erase(inserted);
                return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Refresh});
            }
            subscriber.transport->checks_pending.store(true);
            continue;
        }

        subscriber.missing.erase(id);
        subscriber.sources.emplace(
            id, SubscriberCore::Source{attachment, std::move(*ring_buffer), attachment.floor});
    }

    return {};
}

// Merge one candidate per Publisher. Protect only the selected payload during memcpy;
// validation and C++ facade construction happen after releasing the shared read lease.
template<class Value, class Decode>
Result<Value> read_impl(const std::shared_ptr<SubscriberCore>& subscriber, bool latest,
                        const ReadBatch* batch, Decode decode) {
    const auto operation = latest ? ErrorOperation::GetLast : ErrorOperation::Next;
    return guarded(operation, [&]() -> Result<Value> {
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
                        auto& shared = source.ring_buffer->reader(source.attachment.reader);
                        shared_store(shared.lost_messages, source.losses.lost_messages);
                        shared_store(shared.overwrite_events, source.losses.overwrite_events);
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
                auto& shared = selected->ring_buffer->reader(selected->attachment.reader);
                shared_fetch_add(shared.oversized_messages, 1);
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
            auto value = decode(std::move(bytes), *subscriber, selected->attachment.descriptor, candidate);
            if (!value) {
                return std::unexpected(storage_error(value.error(), operation));
            }

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

// Both decoders run after unprotect, while the local operation lock still owns
// cursor advancement. A rejected image must not silently consume that cursor.
Result<std::unique_ptr<BaseMessage>> decode_message(std::vector<std::byte> bytes,
    const SubscriberCore& subscriber, const Descriptor& publisher, const Metadata& metadata,
    const message::detail::Schema& schema, Factory factory) {
    auto binding = message::detail::adopt_record(std::move(bytes), schema);
    if (!binding) {
        return std::unexpected(binding.error());
    }

    auto value = factory(std::move(*binding));
    MessageAccess::metadata(*value, subscriber.topic, publisher, metadata);
    return value;
}

Result<ImageDelivery> decode_image(std::vector<std::byte> bytes,
    const SubscriberCore& subscriber, const Descriptor& publisher, const Metadata& metadata) {
    const auto capacity = bytes.capacity();
    const auto maximum = static_cast<std::uint64_t>(bytes.size());
    auto image = message::detail::FrameworkAccess::adopt_image(std::move(bytes), subscriber.schema, maximum);
    if (!image) {
        return std::unexpected(image.error());
    }

    return std::make_unique<const ReceivedImage>(ReceivedImage{
        std::move(*image), subscriber.topic, publisher.publisher, subscriber.schema,
        publisher.id, metadata.count, metadata.timestamp, metadata.monotonic_timestamp, capacity});
}

Result<std::unique_ptr<BaseMessage>> read_batch(const std::shared_ptr<SubscriberCore>& subscriber,
    const ReadBatch& batch, const message::detail::Schema& schema, Factory factory) {
    return read_impl<std::unique_ptr<BaseMessage>>(subscriber, false, &batch,
        [&](auto bytes, const auto& core, const auto& publisher, const auto& metadata) {
            return decode_message(std::move(bytes), core, publisher, metadata, schema, factory);
        });
}

Result<std::unique_ptr<BaseMessage>> read(const std::shared_ptr<SubscriberCore>& subscriber,
    bool latest, const message::detail::Schema& schema, Factory factory) {
    return read_impl<std::unique_ptr<BaseMessage>>(subscriber, latest, nullptr,
        [&](auto bytes, const auto& core, const auto& publisher, const auto& metadata) {
            return decode_message(std::move(bytes), core, publisher, metadata, schema, factory);
        });
}

Result<ImageDelivery> read_image_batch(const std::shared_ptr<SubscriberCore>& subscriber, const ReadBatch& batch) {
    return read_impl<ImageDelivery>(subscriber, false, &batch, decode_image);
}

Result<ImageDelivery> ImageSubscriber::get_last() {
    return read_impl<ImageDelivery>(core_, true, nullptr, decode_image);
}

Result<ImageSubscriber> ImageAccess::subscribe(Client& client, std::string topic,
    std::uint64_t schema_hash, SubscriberOptions options) {
    if (!schema_hash) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Subscribe});
    }

    auto core = client.subscribe_raw(std::move(topic), schema_hash, options);
    if (!core) {
        return std::unexpected(core.error());
    }

    return ImageSubscriber(std::move(*core));
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
        control::Bytes payload;
        auto encoded = control::put_string(payload, topic);
        if (!encoded) {
            return std::unexpected(
                vinc::wrap_error(ErrorCode::InvalidArgument, ErrorOperation::Advertise, encoded.error()));
        }
        control::put_u64(payload, schema);
        std::uint32_t present = 0;
        if (options.slots) {
            present |= detail::publisher_slots_present;
        }
        if (options.max_message_bytes) {
            present |= detail::publisher_bytes_present;
        }
        control::put_u32(payload, present);
        if (options.slots) {
            control::put_u32(payload, *options.slots);
        }
        if (options.max_message_bytes) {
            control::put_u64(payload, *options.max_message_bytes);
        }
        auto result = impl_->transport->request(detail::Advertise, std::move(payload));
        if (!result) {
            return std::unexpected(result.error());
        }

        detail::RegistrationCleanup cleanup{*impl_->transport, core->release_node, detail::ClosePublisher};
        control::Reader reader(*result);
#ifdef VINC_MESSAGE_BUS_TESTING
        detail::testing::checkpoint(detail::testing::FaultPoint::DescriptorDecode);
#endif
        const auto requested_slots = reader.u32();
        auto descriptor = detail::decode_descriptor(reader);
        auto targets = detail::decode_notification_targets(reader);
        const bool valid_request = requested_slots != 0 && requested_slots <= detail::maximum_ring_buffer_slots;
        const bool matching_request = !options.slots || requested_slots == *options.slots;
        const bool matching_slots = descriptor.slots == (std::max)(detail::minimum_ring_buffer_slots, requested_slots);
        const bool matching_bytes = !options.max_message_bytes || descriptor.bytes == *options.max_message_bytes;
        if (!targets || !reader.done() || !descriptor.id || descriptor.schema != schema ||
            !valid_request || !matching_request || !matching_slots || !matching_bytes) {
            return std::unexpected(vinc::make_error(ErrorCode::Protocol, ErrorOperation::Advertise));
        }
        core->requested = requested_slots;
        cleanup.id = descriptor.id;
        {
            std::lock_guard lock(impl_->transport->mutex);
            core->notifications = impl_->transport->notification_routes.at(descriptor.id);
        }
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

Result<TopicConnectionsPage> Client::list_topic_connections(ConnectionCursor after, Deadline deadline) {
    return detail::guarded(ErrorOperation::ListTopicConnections, [&]() -> Result<TopicConnectionsPage> {
        if (!detail::valid_cursor(after)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::ListTopicConnections});
        }

        control::Bytes payload;
        control::put_u32(payload, detail::connections_version);
        auto encoded = detail::encode_cursor(payload, after);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }

        auto response = impl_->transport->request(detail::ListTopicConnections, std::move(payload), deadline);
        if (!response) {
            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::ListTopicConnections, response.error()));
        }

        return detail::decode_connections(*response, after);
    });
}

Result<TopicDiagnosticsPage> Client::inspect_topics(std::string after) {
    return detail::guarded(ErrorOperation::InspectTopics, [&]() -> Result<TopicDiagnosticsPage> {
        if (after.size() > detail::maximum_name_bytes || after.find('\0') != std::string::npos) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::InspectTopics});
        }

        control::Bytes payload;
        auto encoded = control::put_string(payload, after);
        if (!encoded) {
            return std::unexpected(wrap_error(ErrorCode::InvalidArgument, ErrorOperation::InspectTopics, encoded.error()));
        }

        auto response = impl_->transport->request(detail::InspectTopics, std::move(payload));
        if (!response) {
            return std::unexpected(wrap_error(ErrorCode::Io, ErrorOperation::InspectTopics, response.error()));
        }

        return detail::decode_diagnostics(*response, after);
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

        detail::RegistrationCleanup cleanup{*impl_->transport, core->release_node, detail::Unsubscribe};
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

Result<void> detail::enable_notifications(const std::shared_ptr<SubscriberCore>& subscriber) {
    if (!subscriber || subscriber->closed.load()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::EnableNotifications});
    }
    control::Bytes payload;
    control::put_u64(payload, subscriber->id);
    auto response = subscriber->transport->request(EnableNotifications, std::move(payload));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (!response->empty()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::EnableNotifications});
    }
    return {};
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
        auto received = client.impl_->transport->receive(std::move(frame));
        if (!received) {
            shutdown(client);
        }
        return received;
    } catch (const std::bad_alloc&) {
        shutdown(client);
        return std::unexpected(vinc::Error{ErrorCode::ResourceExhausted, ErrorOperation::Receive});
    } catch (...) {
        shutdown(client);
        return std::unexpected(vinc::Error{ErrorCode::Io, ErrorOperation::Receive});
    }
}

namespace {
std::vector<std::shared_ptr<const detail::NotificationDestinations>> notification_snapshots(detail::Transport& transport) {
    std::vector<std::shared_ptr<const detail::NotificationDestinations>> snapshots;
    std::lock_guard lock(transport.mutex);
    for (const auto& [_, routes] : transport.notification_routes) {
        auto current = routes->snapshot();
        if (current) {
            snapshots.push_back(std::move(current));
        }
    }
    return snapshots;
}
}

Result<void> detail::RuntimeAccess::progress_control(Client& client) {
    return client.impl_->transport->progress();
}

void detail::RuntimeAccess::progress_notifications(Client& client) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& snapshot : notification_snapshots(*client.impl_->transport)) {
        for (const auto& destination : *snapshot) {
            destination->reopen(now);
        }
    }
}

detail::NotificationDiagnostics detail::RuntimeAccess::notification_diagnostics(Client& client) {
    auto& totals = *client.impl_->transport->notification_totals;
    std::lock_guard lock(totals.mutex);
    return totals.value;
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
