#include "protocol.hpp"
#include "framework_access.hpp"
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>

namespace vinc::bus::detail {
template<class F> auto guarded(F&& f) -> decltype(f()) {
    try { return f(); }
    catch (const std::bad_alloc&) { return std::unexpected(error(ErrorCode::ResourceExhausted, "Bus allocation failed")); }
    catch (const message::AccessError& e) { return std::unexpected(storage_error(e.error())); }
    catch (const std::exception& e) { return std::unexpected(error(ErrorCode::InvalidData, e.what())); }
}
struct Transport {
    struct Pending { std::uint16_t operation; std::optional<control::Frame> response; };
    std::shared_ptr<control::Session> session;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t next_request = 1;
    bool closed = false;
    std::map<std::uint64_t, Pending> pending;
    std::map<std::uint64_t, std::map<std::uint64_t, Attachment>> topology;
    explicit Transport(std::shared_ptr<control::Session> s) : session(std::move(s)) {}
    void fail_locked() { closed = true; session->close(); changed.notify_all(); }
    Result<control::Bytes> request(std::uint16_t operation, control::Bytes bytes) {
        std::unique_lock lock(mutex);
        if (closed) return std::unexpected(error(ErrorCode::Closed, "module Bus session closed"));
        if (pending.size() >= 32 || next_request == UINT64_MAX) return std::unexpected(error(ErrorCode::ResourceExhausted, "too many control requests"));
        const auto id = next_request++; pending.emplace(id, Pending{operation, {}});
        if (!session->send({control::Kind::Request, bus_domain, operation, id, std::move(bytes)})) {
            pending.erase(id); fail_locked(); return std::unexpected(error(ErrorCode::Closed, "send Bus request failed"));
        }
        const bool done = changed.wait_until(lock, control::Clock::now() + std::chrono::seconds(10), [&] { return closed || pending.at(id).response.has_value(); });
        if (!done || closed) {
            pending.erase(id); fail_locked();
            return std::unexpected(error(done ? ErrorCode::Closed : ErrorCode::Timeout, "Bus control session ended; registration is not replayed"));
        }
        auto frame = std::move(*pending.at(id).response); pending.erase(id);
        control::Reader r(frame.payload); const auto status = r.u32();
        if (status) {
            const auto message = r.string();
            if (!r.done() || status > static_cast<std::uint32_t>(ErrorCode::InvalidData)) { fail_locked(); return std::unexpected(error(ErrorCode::Protocol, "invalid Bus response")); }
            return std::unexpected(error(static_cast<ErrorCode>(status), message));
        }
        if (frame.payload.size() < 4) { fail_locked(); return std::unexpected(error(ErrorCode::Protocol, "short Bus response")); }
        return control::Bytes(frame.payload.begin() + 4, frame.payload.end());
    }
    void release(std::uint16_t operation, std::uint64_t id) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (operation == Unsubscribe) topology.erase(id);
            if (closed) return;
            control::Frame f{control::Kind::Event, bus_domain, operation}; control::put_u64(f.payload, id);
            if (!session->send(std::move(f))) fail_locked();
        } catch (...) { session->close(); }
    }
    bool receive(control::Frame frame) {
        std::lock_guard lock(mutex);
        if (closed || frame.domain != bus_domain) return false;
        control::Reader r(frame.payload);
        if (frame.kind == control::Kind::Response) {
            auto found = pending.find(frame.request);
            if (found == pending.end() || found->second.operation != frame.operation || found->second.response) { fail_locked(); return false; }
            const auto status = r.u32();
            if (!status && frame.operation == Subscribe) {
                const auto id = r.u64(); const auto count = r.u32();
                if (!id || count > 32 || topology.contains(id)) { fail_locked(); return false; }
                std::map<std::uint64_t, Attachment> list;
                for (std::uint32_t i = 0; i < count; ++i) { auto a = decode_attachment(r); if (!list.emplace(a.descriptor.id, std::move(a)).second) { fail_locked(); return false; } }
                if (!r.done()) { fail_locked(); return false; }
                topology.emplace(id, std::move(list));
            }
            found->second.response = std::move(frame); changed.notify_all(); return true;
        }
        if (frame.kind != control::Kind::Event || frame.request) { fail_locked(); return false; }
        const auto id = r.u64(); auto target = topology.find(id);
        if (frame.operation == Attach) {
            auto a = decode_attachment(r);
            if (!r.done() || !a.descriptor.id || a.reader >= a.descriptor.readers) { fail_locked(); return false; }
            // A queued topology event can arrive after local unsubscribe.
            if (target != topology.end()) {
                if (target->second.size() >= 32 || !target->second.emplace(a.descriptor.id, std::move(a)).second) { fail_locked(); return false; }
            }
        } else if (frame.operation == Detach) {
            const auto publisher = r.u64(); if (!r.done()) { fail_locked(); return false; }
            if (target != topology.end()) target->second.erase(publisher);
        } else { fail_locked(); return false; }
        return true;
    }
};
struct LoanCore {
    std::shared_ptr<Ring> ring;
    Reservation reservation;
    message::detail::Binding binding;
    std::atomic<bool> active{true};
};
struct PublisherCore {
    std::shared_ptr<Transport> transport;
    std::shared_ptr<Ring> ring;
    std::shared_ptr<NamedCounter> counter;
    std::uint32_t requested = 0;
    std::mutex operation;
    std::atomic<bool> closed{false};
    std::shared_ptr<LoanCore> outstanding;
};
struct SubscriberCore {
    struct Source { Attachment attachment; std::shared_ptr<Ring> ring; std::uint64_t cursor = 0; };
    std::shared_ptr<Transport> transport;
    std::uint64_t id = 0, schema = 0;
    std::string topic;
    std::mutex operation;
    std::atomic<bool> closed{false};
    std::map<std::uint64_t, Source> sources;
};
class MessageAccess {
public:
    static void metadata(BaseMessage& m, const std::string& topic, const Descriptor& publisher, const Metadata& metadata) {
        m._topic_name = topic; m._publisher_name = publisher.publisher;
        m._message_timestamp = metadata.timestamp; m._topic_count = metadata.count; m._publisher_instance = publisher.id;
    }
};
Result<LoanBinding> borrow(const std::shared_ptr<PublisherCore>& p, const message::detail::Schema& schema) {
    return guarded([&]() -> Result<LoanBinding> {
        if (!p) return std::unexpected(error(ErrorCode::Closed, "empty Publisher"));
        std::unique_lock lock(p->operation, std::try_to_lock);
        if (!lock.owns_lock()) return std::unexpected(error(ErrorCode::OperationInProgress, "Publisher operation in progress"));
        if (p->closed.load()) return std::unexpected(error(ErrorCode::Closed, "Publisher closed"));
        if (p->outstanding) return std::unexpected(error(ErrorCode::OutstandingLoan, "previous loan has not been committed"));
        auto loan = std::make_shared<LoanCore>(); loan->ring = p->ring;
        auto reservation = p->ring->reserve(p->closed); if (!reservation) return std::unexpected(reservation.error());
        loan->reservation = *reservation;
        auto binding = message::detail::FrameworkAccess::borrow_record(schema, reservation->bytes, p->ring, p->ring->descriptor.bytes);
        if (!binding) { p->ring->abandon(*reservation); return std::unexpected(storage_error(binding.error())); }
        loan->binding = *binding; p->outstanding = loan;
        return LoanBinding{std::move(loan), std::move(*binding)};
    });
}
void abandon(const std::shared_ptr<LoanCore>& loan) noexcept {
    if (loan && loan->active.exchange(false)) message::detail::release_message(loan->binding.state);
    // Intentionally retain Publisher::outstanding. Dropping a loan is a misuse,
    // not an implicit cancellation that makes another borrow possible.
}
bool usable(const std::shared_ptr<LoanCore>& loan) noexcept { return loan && loan->active.load() && loan->ring->alive(); }
void construction_failed(const std::shared_ptr<PublisherCore>& p, const std::shared_ptr<LoanCore>& loan) noexcept {
    if (!p || !loan) return;
    std::lock_guard lock(p->operation);
    if (p->outstanding == loan) { abandon(loan); p->ring->abandon(loan->reservation); p->outstanding.reset(); }
}
Result<void> commit(const std::shared_ptr<PublisherCore>& p, const std::shared_ptr<LoanCore>& loan, message::detail::Binding binding) {
    return guarded([&]() -> Result<void> {
        if (!p) return std::unexpected(error(ErrorCode::Closed, "empty Publisher"));
        std::unique_lock lock(p->operation, std::try_to_lock);
        if (!lock.owns_lock()) return std::unexpected(error(ErrorCode::OperationInProgress, "Publisher operation in progress"));
        if (p->closed.load()) return std::unexpected(error(ErrorCode::Closed, "Publisher closed"));
        if (!loan || p->outstanding != loan || !usable(loan) || binding.state != loan->binding.state || binding.handle != loan->binding.handle)
            return std::unexpected(error(ErrorCode::InvalidLoan, "loan does not belong to this Publisher"));
        auto image = message::detail::seal_binding(binding); if (!image) return std::unexpected(storage_error(image.error()));
        const auto timestamp = osal::monotonic_nanoseconds();
        auto count = p->counter->next(); if (!count) { loan->active.store(false); return std::unexpected(count.error()); }
        auto published = p->ring->publish(loan->reservation, image->bytes().size(), timestamp, *count);
        loan->active.store(false);
        if (published) p->outstanding.reset();
        return published;
    });
}
Result<void> push(const std::shared_ptr<PublisherCore>& p, message::detail::Binding binding) {
    return guarded([&]() -> Result<void> {
        if (!p) return std::unexpected(error(ErrorCode::Closed, "empty Publisher"));
        std::unique_lock lock(p->operation, std::try_to_lock);
        if (!lock.owns_lock()) return std::unexpected(error(ErrorCode::OperationInProgress, "Publisher operation in progress"));
        if (p->closed.load()) return std::unexpected(error(ErrorCode::Closed, "Publisher closed"));
        if (p->outstanding) return std::unexpected(error(ErrorCode::OutstandingLoan, "previous loan has not been committed"));
        auto usage = message::detail::usage(binding.state); if (!usage) return std::unexpected(storage_error(usage.error()));
        if (usage->compact_bytes > p->ring->descriptor.bytes) return std::unexpected(error(ErrorCode::CapacityExceeded, "message exceeds Publisher byte budget"));
        auto reservation = p->ring->reserve(p->closed); if (!reservation) return std::unexpected(reservation.error());
        struct Rollback { std::shared_ptr<Ring> ring; Reservation r; bool done = false; ~Rollback() { if (!done) ring->abandon(r); } } rollback{p->ring, *reservation};
        auto image = message::detail::publish_binding(binding, reservation->bytes, p->ring);
        if (!image) return std::unexpected(storage_error(image.error()));
        const auto timestamp = osal::monotonic_nanoseconds(); auto count = p->counter->next(); if (!count) return std::unexpected(count.error());
        auto published = p->ring->publish(*reservation, image->bytes().size(), timestamp, *count);
        rollback.done = bool(published); return published;
    });
}
void close(const std::shared_ptr<PublisherCore>& p) noexcept {
    if (!p || p->closed.exchange(true)) return;
    p->ring->retire(); // wakes a reserve operation waiting on a live reader
    std::lock_guard lock(p->operation);
    abandon(p->outstanding);
    if (p->outstanding) p->outstanding->ring.reset();
    p->outstanding.reset();
    p->transport->release(ClosePublisher, p->ring->descriptor.id);
}
void close(const std::shared_ptr<SubscriberCore>& s) noexcept {
    if (!s || s->closed.exchange(true)) return;
    std::lock_guard lock(s->operation); // read operation's guard is released before unsubscribe
    s->sources.clear(); s->transport->release(Unsubscribe, s->id);
}
PublisherStats stats(const std::shared_ptr<PublisherCore>& p) { return p ? p->ring->stats(p->requested) : PublisherStats{}; }
Result<void> refresh(SubscriberCore& s) {
    std::map<std::uint64_t, Attachment> topology;
    { std::lock_guard lock(s.transport->mutex);
      if (s.transport->closed) return std::unexpected(error(ErrorCode::Closed, "module Bus session closed"));
      auto t = s.transport->topology.find(s.id); if (t == s.transport->topology.end()) return std::unexpected(error(ErrorCode::Closed, "subscription retired"));
      topology = t->second; }
    for (auto it = s.sources.begin(); it != s.sources.end();) {
        if (!topology.contains(it->first)) it = s.sources.erase(it); else ++it;
    }
    for (auto& [id, a] : topology) {
        if (s.sources.contains(id)) continue;
        if (a.descriptor.schema != s.schema || a.reader >= a.descriptor.readers) return std::unexpected(error(ErrorCode::Protocol, "invalid attached publisher"));
        auto ring = Ring::open(a.descriptor, false);
        if (!ring) {
            // Retirement can race a previously received Attach event. If the
            // detach has arrived, ignore it; otherwise surface the real OS error.
            std::lock_guard lock(s.transport->mutex);
            auto t = s.transport->topology.find(s.id);
            if (t == s.transport->topology.end() || !t->second.contains(id) || ring.error().code == ErrorCode::Closed) continue;
            return std::unexpected(ring.error());
        }
        s.sources.emplace(id, SubscriberCore::Source{a, std::move(*ring), a.floor});
    }
    return {};
}
Result<std::unique_ptr<BaseMessage>> read(const std::shared_ptr<SubscriberCore>& s, bool latest, const message::detail::Schema& schema, Factory factory) {
    return guarded([&]() -> Result<std::unique_ptr<BaseMessage>> {
        if (!s) return std::unexpected(error(ErrorCode::Closed, "empty Subscriber"));
        std::unique_lock lock(s->operation, std::try_to_lock);
        if (!lock.owns_lock()) return std::unexpected(error(ErrorCode::OperationInProgress, "Subscriber operation in progress"));
        auto updated = refresh(*s); if (!updated) return std::unexpected(updated.error());
        for (;;) {
            if (s->closed.load()) return std::unexpected(error(ErrorCode::Closed, "Subscriber closed"));
            SubscriberCore::Source* selected = nullptr; Metadata candidate;
            for (auto& [_, source] : s->sources) {
                auto m = source.ring->candidate(source.cursor, latest); if (!m) continue;
                if (!selected || preferred(*m, source.attachment.descriptor.rank, candidate, selected->attachment.descriptor.rank, latest)) {
                    selected = &source; candidate = *m;
                }
            }
            if (!selected) return std::unexpected(error(latest ? ErrorCode::NoPublishedData : ErrorCode::NoNewData, latest ? "no published message" : "no new message"));
            // Allocation and zero-initialization are outside the read protection.
            std::vector<std::byte> bytes(static_cast<std::size_t>(candidate.size));
            auto& ring = *selected->ring; const auto reader = selected->attachment.reader;
            if (!ring.protect(candidate, reader)) continue;
            std::memcpy(bytes.data(), ring.payload(candidate).data(), bytes.size());
            ring.unprotect(reader); // Only the bounded payload copy held the lease.
            auto binding = message::detail::adopt_record(std::move(bytes), schema);
            if (!binding) return std::unexpected(storage_error(binding.error()));
            auto value = factory(std::move(*binding));
            MessageAccess::metadata(*value, s->topic, selected->attachment.descriptor, candidate);
            if (!latest) selected->cursor = candidate.sequence;
            return value;
        }
    });
}
}

namespace vinc::bus {
struct Client::Impl { std::shared_ptr<detail::Transport> transport; };
Client::Client(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
Client::~Client() = default;
Result<std::shared_ptr<detail::PublisherCore>> Client::advertise_raw(std::string topic, std::uint64_t schema, PublisherOptions options) {
    return detail::guarded([&]() -> Result<std::shared_ptr<detail::PublisherCore>> {
        // Allocate local ownership before the remote mutation so failures can clean up.
        auto core = std::make_shared<detail::PublisherCore>(); core->transport = impl_->transport; core->requested = options.slots;
        control::Bytes b; control::put_string(b, topic); control::put_u64(b, schema); control::put_u32(b, options.slots); control::put_u64(b, options.max_message_bytes);
        auto result = impl_->transport->request(detail::Advertise, std::move(b)); if (!result) return std::unexpected(result.error());
        control::Reader reader(*result); auto descriptor = detail::decode_descriptor(reader);
        if (!reader.done() || descriptor.schema != schema) return std::unexpected(detail::error(ErrorCode::Protocol, "invalid publisher response"));
        struct Cleanup { std::shared_ptr<detail::Transport> t; std::uint64_t id; bool done = false; ~Cleanup() { if (!done) t->release(detail::ClosePublisher, id); } } cleanup{impl_->transport, descriptor.id};
        auto ring = detail::Ring::open(descriptor, true); if (!ring) return std::unexpected(ring.error()); core->ring = std::move(*ring);
        auto counter = detail::NamedCounter::open(descriptor.counter); if (!counter) return std::unexpected(counter.error()); core->counter = std::move(*counter);
        cleanup.done = true; return core;
    });
}
Result<std::shared_ptr<detail::SubscriberCore>> Client::subscribe_raw(std::string topic, std::uint64_t schema, SubscriberOptions options) {
    return detail::guarded([&]() -> Result<std::shared_ptr<detail::SubscriberCore>> {
        auto core = std::make_shared<detail::SubscriberCore>(); core->transport = impl_->transport; core->schema = schema; core->topic = topic;
        control::Bytes b; control::put_string(b, topic); control::put_u64(b, schema); control::put_u32(b, static_cast<std::uint32_t>(options.start));
        auto result = impl_->transport->request(detail::Subscribe, std::move(b)); if (!result) return std::unexpected(result.error());
        control::Reader r(*result); core->id = r.u64(); const auto count = r.u32();
        if (!core->id || count > 32) return std::unexpected(detail::error(ErrorCode::Protocol, "invalid subscription response"));
        for (std::uint32_t i = 0; i < count; ++i) (void)detail::decode_attachment(r);
        if (!r.done()) { detail::close(core); return std::unexpected(detail::error(ErrorCode::Protocol, "invalid subscription descriptors")); }
        return core;
    });
}
std::shared_ptr<Client> detail::RuntimeAccess::create(std::shared_ptr<control::Session> session) {
    auto p = std::make_shared<Client::Impl>(); p->transport = std::make_shared<Transport>(std::move(session));
    return std::shared_ptr<Client>(new Client(std::move(p)));
}
bool detail::RuntimeAccess::receive(Client& client, control::Frame frame) {
    try { return client.impl_->transport->receive(std::move(frame)); }
    catch (...) { shutdown(client); return false; }
}
void detail::RuntimeAccess::shutdown(Client& client) noexcept {
    auto& t = *client.impl_->transport; std::lock_guard lock(t.mutex); t.fail_locked();
}
std::vector<detail::Attachment> detail::RuntimeAccess::inspect_attachments(Client& client) {
    auto& t = *client.impl_->transport; std::lock_guard lock(t.mutex);
    std::vector<Attachment> result;
    for (const auto& [_, sources] : t.topology) for (const auto& [id, source] : sources) result.push_back(source);
    return result;
}
}
