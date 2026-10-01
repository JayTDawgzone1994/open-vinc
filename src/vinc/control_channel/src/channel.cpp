// Shared Socket reactor for session handshake, heartbeat and bounded control queues.
// Public calls enqueue/dequeue under a peer lock; one worker owns actual socket I/O.

#include <vinc/control/channel.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace vinc::control {
namespace detail {
constexpr std::size_t queue_bytes_limit = 256 * 1024;
constexpr std::size_t queue_count_limit = 64;
// Peer::mutex protects both public queue access and reactor state.
// The weak Engine link lets a retained Session fail cleanly after Service destruction.
struct Peer {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::weak_ptr<Engine> engine;
    osal::Socket socket;
    std::optional<osal::SocketRegistration> registration;
    Options options;
    State state = State::Handshaking;
    bool closing = false;
    std::optional<Error> failure;
    Deadline handshake_end, last_heartbeat, next_heartbeat;
    std::uint64_t sent_sequence = 0, received_sequence = 0;
    Decoder decoder;
    std::deque<Bytes> outgoing;
    std::deque<Frame> incoming;
    std::size_t outgoing_bytes = 0, incoming_bytes = 0;
    Bytes active;
    std::size_t offset = 0;
    bool active_application = false;
};
struct Engine {
    explicit Engine(osal::SocketPoller p, std::size_t limit) : poller(std::move(p)), max_sessions(limit) {}
    osal::SocketPoller poller;
    std::mutex mutex;
    std::vector<std::shared_ptr<Peer>> peers;
    std::size_t max_sessions;
    bool stopping = false;
    std::jthread worker;
};
}
namespace {
Error error(ErrorCode code, std::string message) { return {code, std::move(message), {}}; }
void wake(detail::Peer& peer) { if (auto engine = peer.engine.lock()) (void)engine->poller.wake(); }
void fail(detail::Peer& peer, Error reason) {
    if (!peer.failure) peer.failure = std::move(reason);
    peer.closing = true;
}
void dispose(detail::Engine& engine, detail::Peer& peer) {
    if (peer.registration) { (void)engine.poller.remove(*peer.registration); peer.registration.reset(); }
    (void)peer.socket.close();
    peer.state = State::Closed;
    peer.outgoing.clear(); peer.active.clear(); peer.outgoing_bytes = 0;
    peer.changed.notify_all();
}
Frame hello(const Options& options) {
    Frame result{Kind::Hello};
    put_u32(result.payload, static_cast<std::uint32_t>(options.role));
    put_u64(result.payload, options.generation); put_string(result.payload, options.token);
    put_u32(result.payload, static_cast<std::uint32_t>(options.heartbeat_interval.count()));
    put_u32(result.payload, static_cast<std::uint32_t>(options.heartbeat_timeout.count()));
    return result;
}
// Validate the expected role, generation and token before accepting application frames.
// Only heartbeat frames refresh liveness; ordinary traffic is not a replacement heartbeat.
void incoming(detail::Peer& peer, Frame frame, Deadline now) {
    if (frame.kind == Kind::Hello) {
        Reader reader(frame.payload);
        const auto role = reader.u32(); const auto generation = reader.u64(); const auto token = reader.string();
        const auto interval = reader.u32(); const auto timeout = reader.u32();
        const auto expected_role = peer.options.role == Role::Central ? Role::Module : Role::Central;
        if (peer.state != State::Handshaking || frame.domain || frame.operation || frame.request ||
            role != static_cast<std::uint32_t>(expected_role) || generation != peer.options.generation ||
            token != peer.options.token || !reader.done() ||
            interval != peer.options.heartbeat_interval.count() || timeout != peer.options.heartbeat_timeout.count()) {
            fail(peer, error(ErrorCode::Protocol, "session handshake mismatch")); return;
        }
        peer.state = State::Open; peer.last_heartbeat = now; peer.next_heartbeat = now;
        peer.changed.notify_all(); return;
    }
    if (peer.state != State::Open) { fail(peer, error(ErrorCode::Protocol, "message before handshake")); return; }
    if (frame.kind == Kind::Heartbeat) {
        Reader reader(frame.payload); const auto sequence = reader.u64();
        if (frame.domain || frame.operation || frame.request || !reader.done() || sequence <= peer.received_sequence) {
            fail(peer, error(ErrorCode::Protocol, "invalid heartbeat")); return;
        }
        peer.received_sequence = sequence; peer.last_heartbeat = now; return;
    }
    if (!frame.domain || ((frame.kind == Kind::Request || frame.kind == Kind::Response) && !frame.request) ||
        (frame.kind == Kind::Event && frame.request)) {
        fail(peer, error(ErrorCode::Protocol, "invalid application envelope")); return;
    }
    const auto size = header_size + frame.payload.size();
    if (peer.incoming.size() >= detail::queue_count_limit || peer.incoming_bytes + size > detail::queue_bytes_limit) {
        fail(peer, error(ErrorCode::Backpressure, "receive queue exhausted")); return;
    }
    peer.incoming_bytes += size; peer.incoming.push_back(std::move(frame)); peer.changed.notify_all();
}
void advance(detail::Engine& engine, detail::Peer& peer, Deadline now) {
    if (peer.state == State::Closed) return;
    if (peer.closing) { dispose(engine, peer); return; }
    if (!peer.registration) {
        auto registration = engine.poller.add(peer.socket, osal::SocketEvents::Readable);
        if (!registration) { fail(peer, {ErrorCode::Io, "register socket", registration.error()}); dispose(engine, peer); return; }
        peer.registration = *registration;
    }
    // Bound work per peer so a busy socket cannot monopolize the shared reactor.
    std::array<std::byte, 4096> bytes;
    for (int budget = 0; budget < 8 && !peer.closing; ++budget) {
        auto got = peer.socket.try_receive(bytes);
        if (!got) {
            if (got.error().code != osal::ErrorCode::WouldBlock) fail(peer, {ErrorCode::Io, "receive socket", got.error()});
            break;
        }
        if (got->eof) { fail(peer, error(ErrorCode::Closed, "peer disconnected")); break; }
        auto frames = peer.decoder.feed(std::span<const std::byte>(bytes).first(got->bytes));
        if (!frames) { fail(peer, error(ErrorCode::Protocol, frames.error())); break; }
        for (auto& frame : *frames) { incoming(peer, std::move(frame), now); if (peer.closing) break; }
    }
    if (peer.state == State::Handshaking && now >= peer.handshake_end)
        fail(peer, error(ErrorCode::Timeout, "handshake timeout"));
    if (peer.state == State::Open && now - peer.last_heartbeat >= peer.options.heartbeat_timeout)
        fail(peer, error(ErrorCode::Timeout, "heartbeat timeout"));
    for (int budget = 0; budget < 8 && !peer.closing; ++budget) {
        if (peer.active.empty()) {
            peer.offset = 0; peer.active_application = false;
            // Prioritize heartbeat between complete frames. Never interleave bytes into a
            // partially sent application frame, because the receiver sees one ordered stream.
            if (peer.state == State::Open && now >= peer.next_heartbeat) {
                Frame heartbeat{Kind::Heartbeat}; put_u64(heartbeat.payload, ++peer.sent_sequence);
                peer.active = encode(heartbeat); peer.next_heartbeat = now + peer.options.heartbeat_interval;
            } else if (peer.state == State::Open && !peer.outgoing.empty()) {
                peer.active = std::move(peer.outgoing.front()); peer.outgoing.pop_front(); peer.active_application = true;
            } else break;
        }
        auto sent = peer.socket.try_send(std::span<const std::byte>(peer.active).subspan(peer.offset));
        if (!sent) {
            if (sent.error().code != osal::ErrorCode::WouldBlock) fail(peer, {ErrorCode::Io, "send socket", sent.error()});
            break;
        }
        if (!*sent) break;
        peer.offset += *sent;
        if (peer.offset == peer.active.size()) {
            if (peer.active_application) peer.outgoing_bytes -= peer.active.size();
            peer.active.clear(); peer.offset = 0;
        }
    }
    if (peer.closing) { dispose(engine, peer); return; }
    auto interests = osal::SocketEvents::Readable;
    if (!peer.active.empty() || (peer.state == State::Open && !peer.outgoing.empty())) interests = interests | osal::SocketEvents::Writable;
    auto changed = engine.poller.modify(*peer.registration, interests);
    if (!changed) { fail(peer, {ErrorCode::Io, "modify socket registration", changed.error()}); dispose(engine, peer); }
}
// Use the nearest session deadline as the poll timeout. Each peer has a work budget
// so one busy connection cannot prevent the others from advancing.
void run(detail::Engine& engine, std::stop_token stop) noexcept {
    try {
        while (!stop.stop_requested()) {
            std::vector<std::shared_ptr<detail::Peer>> peers;
            { std::lock_guard lock(engine.mutex); peers = engine.peers; }
            auto deadline = (Deadline::max)();
            for (auto& peer : peers) {
                std::lock_guard lock(peer->mutex);
                advance(engine, *peer, Clock::now());
                if (peer->state == State::Handshaking) deadline = (std::min)(deadline, peer->handshake_end);
                if (peer->state == State::Open) {
                    deadline = (std::min)(deadline, peer->last_heartbeat + peer->options.heartbeat_timeout);
                    // A partially sent frame cannot be preempted. Do not spin on an overdue heartbeat.
                    if (peer->active.empty()) deadline = (std::min)(deadline, peer->next_heartbeat);
                }
            }
            {
                std::lock_guard lock(engine.mutex);
                std::erase_if(engine.peers, [](const auto& p) { std::lock_guard peer_lock(p->mutex); return p->state == State::Closed; });
            }
            std::array<osal::SocketReadyEvent, 256> ready;
            auto events = engine.poller.wait_until(ready, deadline, stop);
            if (!events && events.error().code != osal::ErrorCode::Timeout && events.error().code != osal::ErrorCode::Cancelled) {
                for (auto& peer : peers) { std::lock_guard lock(peer->mutex); fail(*peer, {ErrorCode::Io, "poller failed", events.error()}); }
                break;
            }
        }
    } catch (...) { /* Close every session below; exceptions must never escape the I/O thread. */ }
    std::lock_guard lock(engine.mutex); engine.stopping = true;
    for (auto& peer : engine.peers) {
        std::lock_guard peer_lock(peer->mutex);
        fail(*peer, error(ErrorCode::Closed, "control service stopped")); dispose(engine, *peer);
    }
    engine.peers.clear();
}
}
Session::Session(std::shared_ptr<detail::Peer> peer) : peer_(std::move(peer)) {}
State Session::state() const { std::lock_guard lock(peer_->mutex); return peer_->state; }
std::optional<Error> Session::error() const { std::lock_guard lock(peer_->mutex); return peer_->failure; }
Result<void> Session::wait_open(Deadline deadline) {
    std::unique_lock lock(peer_->mutex);
    if (!peer_->changed.wait_until(lock, deadline, [&] { return peer_->state != State::Handshaking; }))
        return std::unexpected(control::error(ErrorCode::Timeout, "waiting for handshake"));
    if (peer_->state != State::Open) return std::unexpected(*peer_->failure);
    return {};
}
Result<void> Session::send(Frame frame) {
    if ((frame.kind != Kind::Request && frame.kind != Kind::Response && frame.kind != Kind::Event) || !frame.domain ||
        frame.payload.size() > max_payload || (frame.kind == Kind::Event ? frame.request != 0 : frame.request == 0))
        return std::unexpected(control::error(ErrorCode::InvalidArgument, "invalid application frame"));
    auto bytes = encode(frame);
    { std::lock_guard lock(peer_->mutex);
      if (peer_->closing || peer_->state != State::Open) return std::unexpected(control::error(ErrorCode::Closed, "session is not open"));
      if (peer_->outgoing.size() >= detail::queue_count_limit || peer_->outgoing_bytes + bytes.size() > detail::queue_bytes_limit)
          return std::unexpected(control::error(ErrorCode::Backpressure, "send queue exhausted"));
      peer_->outgoing.push_back(std::move(bytes)); peer_->outgoing_bytes += peer_->outgoing.back().size(); }
    wake(*peer_); return {};
}
Result<std::optional<Frame>> Session::try_receive() {
    std::lock_guard lock(peer_->mutex);
    if (!peer_->incoming.empty()) {
        auto result = std::move(peer_->incoming.front()); peer_->incoming.pop_front();
        peer_->incoming_bytes -= header_size + result.payload.size(); return std::optional<Frame>(std::move(result));
    }
    if (peer_->state == State::Closed) return std::unexpected(*peer_->failure);
    return std::optional<Frame>{};
}
Result<Frame> Session::receive_until(Deadline deadline) {
    std::unique_lock lock(peer_->mutex);
    if (!peer_->changed.wait_until(lock, deadline, [&] { return !peer_->incoming.empty() || peer_->state == State::Closed; }))
        return std::unexpected(control::error(ErrorCode::Timeout, "waiting for message"));
    if (peer_->incoming.empty()) return std::unexpected(*peer_->failure);
    auto frame = std::move(peer_->incoming.front()); peer_->incoming.pop_front();
    peer_->incoming_bytes -= header_size + frame.payload.size(); return frame;
}
void Session::close() {
    { std::lock_guard lock(peer_->mutex); fail(*peer_, control::error(ErrorCode::Closed, "session closed locally")); }
    wake(*peer_);
}
Service::Service(std::shared_ptr<detail::Engine> engine) : engine_(std::move(engine)) {}
Result<std::unique_ptr<Service>> Service::create(std::size_t max_sessions) {
    if (!max_sessions || max_sessions > 4096) return std::unexpected(error(ErrorCode::InvalidArgument, "session limit must be 1..4096"));
    auto poller = osal::SocketPoller::create(max_sessions);
    if (!poller) return std::unexpected(Error{ErrorCode::Io, "create poller", poller.error()});
    auto engine = std::make_shared<detail::Engine>(std::move(*poller), max_sessions);
    auto result = std::unique_ptr<Service>(new Service(engine));
    engine->worker = std::jthread([raw = engine.get()](std::stop_token stop) { run(*raw, stop); });
    return result;
}
Service::~Service() {
    engine_->worker.request_stop();
    (void)engine_->poller.wake();
    if (engine_->worker.joinable()) engine_->worker.join();
}
Result<std::shared_ptr<Session>> Service::attach(osal::Socket socket, Options options) {
    if (!socket || options.token.empty() || options.token.size() > 256 || !options.generation ||
        options.heartbeat_interval.count() <= 0 || options.heartbeat_timeout <= options.heartbeat_interval ||
        options.heartbeat_timeout > std::chrono::hours(24) || options.handshake_timeout.count() <= 0 ||
        options.handshake_timeout > std::chrono::hours(24) ||
        (options.role != Role::Central && options.role != Role::Module))
        return std::unexpected(error(ErrorCode::InvalidArgument, "invalid session options"));
    auto peer = std::make_shared<detail::Peer>();
    peer->socket = std::move(socket); peer->options = std::move(options); peer->engine = engine_;
    peer->handshake_end = Clock::now() + peer->options.handshake_timeout;
    peer->active = encode(hello(peer->options));
    auto session = std::shared_ptr<Session>(new Session(peer));
    { std::lock_guard lock(engine_->mutex);
      if (engine_->stopping) return std::unexpected(error(ErrorCode::Closed, "service stopped"));
      if (engine_->peers.size() >= engine_->max_sessions) return std::unexpected(error(ErrorCode::Backpressure, "session limit reached"));
      engine_->peers.push_back(peer); }
    wake(*peer); return session;
}
} // namespace vinc::control
