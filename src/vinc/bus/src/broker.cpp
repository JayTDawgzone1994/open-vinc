#include "protocol.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace vinc::bus::detail {
struct Broker::Impl {
    struct Peer { std::shared_ptr<control::Session> session; std::uint64_t last_request = 0; };
    struct Sub { std::uint64_t id, owner; std::uint32_t index; };
    struct Topic {
        std::uint64_t schema = 0;
        std::shared_ptr<NamedCounter> counter;
        std::map<std::uint64_t, std::shared_ptr<Ring>> publishers;
        std::map<std::uint64_t, Sub> subscribers;
        std::vector<bool> used;
    };
    struct Work { std::uint64_t owner; std::string name; std::shared_ptr<control::Session> session; control::Frame frame; bool exit = false; };
    Limits limits;
    std::uint64_t next_id = 1, allocated = 0;
    std::map<std::string, Topic> topics;
    std::map<std::uint64_t, Peer> peers;
    std::mutex mutex; std::condition_variable cv;
    std::deque<Work> work;
    bool stopping = false;
    std::size_t pending_work = 0;
    std::thread worker;
    explicit Impl(Limits l) : limits(l) {
        if (!l.max_topics || !l.publishers_per_topic || l.publishers_per_topic > 32 || !l.subscribers_per_topic || l.subscribers_per_topic > 4096 || l.max_shared_bytes < 1024)
            throw std::invalid_argument("invalid Bus limits");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; cv.notify_all(); }
        worker.join();
    }
    void send(std::uint64_t owner, control::Frame f) {
        auto p = peers.find(owner); if (p == peers.end()) return;
        if (!p->second.session->send(std::move(f))) p->second.session->close();
    }
    void response(const Work& w, Result<control::Bytes> body) {
        control::Frame frame{control::Kind::Response, bus_domain, w.frame.operation, w.frame.request};
        control::put_u32(frame.payload, body ? 0 : static_cast<std::uint32_t>(body.error().code));
        if (body) frame.payload.insert(frame.payload.end(), body->begin(), body->end());
        else control::put_string(frame.payload, body.error().message);
        send(w.owner, std::move(frame));
    }
    Result<Topic*> topic(const std::string& name, std::uint64_t schema) {
        if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos || !schema)
            return std::unexpected(error(ErrorCode::InvalidArgument, "invalid topic or schema"));
        auto it = topics.find(name);
        if (it != topics.end()) {
            if (it->second.schema != schema) return std::unexpected(error(ErrorCode::TypeMismatch, "topic already uses another schema"));
            return &it->second;
        }
        if (topics.size() >= limits.max_topics || allocated > limits.max_shared_bytes - sizeof(SharedWord))
            return std::unexpected(error(ErrorCode::ResourceExhausted, "topic limit reached"));
        auto counter = NamedCounter::create(shared_name('t')); if (!counter) return std::unexpected(counter.error());
        Topic t; t.schema = schema; t.counter = std::move(*counter); t.used.resize(limits.subscribers_per_topic);
        auto [added, _] = topics.emplace(name, std::move(t)); allocated += sizeof(SharedWord); return &added->second;
    }
    void prune() {
        for (auto it = topics.begin(); it != topics.end();) {
            if (it->second.publishers.empty() && it->second.subscribers.empty()) { allocated -= sizeof(SharedWord); it = topics.erase(it); }
            else ++it;
        }
    }
    void remove_publisher(Topic& t, std::map<std::uint64_t, std::shared_ptr<Ring>>::iterator it) {
        auto ring = it->second; ring->retire();
        for (const auto& [_, sub] : t.subscribers) {
            control::Frame f{control::Kind::Event, bus_domain, Detach};
            control::put_u64(f.payload, sub.id); control::put_u64(f.payload, it->first); send(sub.owner, std::move(f));
        }
        allocated -= Ring::control_bytes(ring->descriptor.slots, ring->descriptor.readers) + ring->descriptor.slots * Ring::slot_stride(ring->descriptor.bytes);
        t.publishers.erase(it);
    }
    void remove_subscriber(Topic& t, std::map<std::uint64_t, Sub>::iterator it) {
        for (auto& [_, p] : t.publishers) p->clear_reader(it->second.index);
        t.used[it->second.index] = false; t.subscribers.erase(it);
    }
    void exited(std::uint64_t owner) {
        // The owner is a unique process generation, never a reusable OS PID.
        for (auto& [_, t] : topics) {
            for (auto i = t.subscribers.begin(); i != t.subscribers.end();) {
                if (i->second.owner == owner) { auto old = i++; remove_subscriber(t, old); } else ++i;
            }
            for (auto i = t.publishers.begin(); i != t.publishers.end();) {
                if (i->second->descriptor.owner == owner) { auto old = i++; remove_publisher(t, old); } else ++i;
            }
        }
        peers.erase(owner); prune();
    }
    Result<control::Bytes> execute(const Work& w) {
        control::Reader r(w.frame.payload); control::Bytes body;
        if (w.frame.operation == Advertise) {
            const auto name = r.string(); const auto schema = r.u64();
            const auto requested = r.u32(); const auto bytes = r.u64();
            if (!r.done() || !requested || requested > 65536 || bytes < 64 || bytes > SIZE_MAX - 7 || bytes > limits.max_shared_bytes || w.name.size() > 256)
                return std::unexpected(error(ErrorCode::InvalidArgument, "invalid publisher dimensions"));
            const auto slots = (std::max)(2U, requested);
            if (Ring::slot_stride(bytes) > limits.max_shared_bytes / slots) return std::unexpected(error(ErrorCode::ResourceExhausted, "shared memory budget exceeded"));
            const auto data_size = slots * Ring::slot_stride(bytes);
            const auto metadata_size = Ring::control_bytes(slots, limits.subscribers_per_topic);
            if (data_size > UINT64_MAX - metadata_size) return std::unexpected(error(ErrorCode::ResourceExhausted, "shared memory size overflow"));
            const auto needed = data_size + metadata_size;
            if (needed > limits.max_shared_bytes || allocated > limits.max_shared_bytes - needed)
                return std::unexpected(error(ErrorCode::ResourceExhausted, "shared memory budget exceeded"));
            auto found = topic(name, schema); if (!found) return std::unexpected(found.error()); auto& t = **found;
            if (t.publishers.size() >= limits.publishers_per_topic || next_id == UINT64_MAX || allocated > limits.max_shared_bytes - needed)
                return std::unexpected(error(ErrorCode::ResourceExhausted, "publisher limit reached"));
            Descriptor d; d.id = next_id++; d.owner = w.owner; d.rank = d.id; d.schema = schema; d.slots = slots; d.bytes = bytes;
            d.readers = limits.subscribers_per_topic; d.control = shared_name('c'); d.data = shared_name('d'); d.counter = t.counter->name; d.publisher = w.name;
            auto ring = Ring::create(d); if (!ring) return std::unexpected(ring.error());
            encode_descriptor(body, d); t.publishers.emplace(d.id, *ring); allocated += needed;
            for (const auto& [_, sub] : t.subscribers) {
                control::Frame f{control::Kind::Event, bus_domain, Attach};
                control::put_u64(f.payload, sub.id); encode_attachment(f.payload, {d, sub.index, 0}); send(sub.owner, std::move(f));
            }
        } else if (w.frame.operation == Subscribe) {
            const auto name = r.string(); const auto schema = r.u64(); const auto start = r.u32();
            if (!r.done() || start > 1) return std::unexpected(error(ErrorCode::Protocol, "invalid subscription"));
            auto found = topic(name, schema); if (!found) return std::unexpected(found.error()); auto& t = **found;
            auto free = std::find(t.used.begin(), t.used.end(), false);
            if (free == t.used.end() || next_id == UINT64_MAX) return std::unexpected(error(ErrorCode::ResourceExhausted, "subscription limit reached"));
            const auto index = static_cast<std::uint32_t>(free - t.used.begin()); const auto id = next_id++;
            control::put_u64(body, id); control::put_u32(body, static_cast<std::uint32_t>(t.publishers.size()));
            for (const auto& [_, p] : t.publishers) encode_attachment(body, {p->descriptor, index, start ? shared_load(p->header().head) : 0});
            if (body.size() + 4 > control::max_payload) return std::unexpected(error(ErrorCode::ResourceExhausted, "subscription descriptor exceeds protocol bound"));
            t.subscribers.emplace(id, Sub{id, w.owner, index}); *free = true;
        } else if (w.frame.operation == ClosePublisher || w.frame.operation == Unsubscribe) {
            const auto id = r.u64(); if (!r.done()) return std::unexpected(error(ErrorCode::Protocol, "invalid close request"));
            for (auto& [_, t] : topics) {
                if (w.frame.operation == ClosePublisher) {
                    auto p = t.publishers.find(id); if (p != t.publishers.end() && p->second->descriptor.owner == w.owner) { remove_publisher(t, p); break; }
                } else {
                    auto s = t.subscribers.find(id); if (s != t.subscribers.end() && s->second.owner == w.owner) { remove_subscriber(t, s); break; }
                }
            }
        } else return std::unexpected(error(ErrorCode::Protocol, "unsupported Bus operation"));
        return body;
    }
    void run() {
        for (;;) {
            Work w;
            { std::unique_lock lock(mutex); cv.wait(lock, [&] { return stopping || !work.empty(); });
              if (work.empty()) break; w = std::move(work.front()); work.pop_front(); }
            struct Completed {
                Impl& self;
                ~Completed() { std::lock_guard lock(self.mutex); --self.pending_work; self.cv.notify_all(); }
            } completed{*this};
            try {
                if (w.exit) { exited(w.owner); continue; }
                auto [peer, added] = peers.try_emplace(w.owner, Peer{w.session});
                if (peer->second.session != w.session || w.frame.domain != bus_domain) { w.session->close(); continue; }
                if (w.frame.kind == control::Kind::Request) {
                    if (!w.frame.request || w.frame.request <= peer->second.last_request) { w.session->close(); continue; }
                    peer->second.last_request = w.frame.request;
                    response(w, execute(w));
                } else if (w.frame.kind == control::Kind::Event && !w.frame.request && (w.frame.operation == ClosePublisher || w.frame.operation == Unsubscribe)) {
                    if (!execute(w)) w.session->close();
                } else w.session->close();
                prune();
            } catch (...) { if (w.session) w.session->close(); }
        }
        topics.clear(); peers.clear();
    }
};
Broker::Broker(Limits l) : impl_(std::make_unique<Impl>(l)) {}
Broker::~Broker() = default;
bool Broker::submit(std::uint64_t owner, std::string name, std::shared_ptr<control::Session> session, control::Frame frame) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->work.size() >= 256) return false;
    impl_->work.push_back({owner, std::move(name), std::move(session), std::move(frame), false}); ++impl_->pending_work; impl_->cv.notify_all(); return true;
}
void Broker::exited(std::uint64_t owner) {
    std::lock_guard lock(impl_->mutex);
    // Cleanup is never discarded due to the application request queue limit.
    impl_->work.push_back({owner, {}, {}, {}, true}); ++impl_->pending_work; impl_->cv.notify_all();
}
bool Broker::drain_until(control::Deadline deadline) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_until(lock, deadline, [&] { return impl_->pending_work == 0; });
}
}
