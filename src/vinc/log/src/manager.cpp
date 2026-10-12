#include <vinc/log/detail/manager.hpp>
#include <vinc/log/detail/files.hpp>
#include <vinc/log/detail/transport.hpp>
#include <vinc/osal/path.hpp>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace vinc::log::detail {
namespace {
std::optional<Level> parse_level(std::string_view text) {
    if (text == "error") {
        return Level::Error;
    }
    if (text == "warn") {
        return Level::Warn;
    }
    if (text == "info") {
        return Level::Info;
    }
    if (text == "debug") {
        return Level::Debug;
    }
    return {};
}
}

struct Manager::Impl {
    struct Peer {
        std::uint64_t mid = 0;
        std::uint64_t pid = 0;
        std::string name;
        std::string endpoint;
        control::Options options;
        std::optional<osal::Socket> listener;
        std::shared_ptr<control::Session> session;
        std::uint64_t last_sequence = 0;
        std::uint64_t rejected = 0;
        std::uint64_t dropped = 0;
        std::uint64_t disk_dropped = 0;
        std::uint64_t raw_dropped_bytes = 0;
        std::uint64_t raw_dropped_chunks = 0;
        bool fenced = false;
        bool disconnect_reported = false;
        bool exited = false;
        bool level_pending = true;
        Level level = Level::Info;

        ~Peer() {
            if (session) {
                session->close();
            }
            listener.reset();
#ifndef _WIN32
            auto path = osal::path_from_utf8(endpoint);
            if (path) {
                std::error_code ignored;
                std::filesystem::remove(*path, ignored);
            }
#endif
        }
    };

    struct Pending {
        std::shared_ptr<Record> record;
        std::size_t bytes;
    };

    control::Service& service;
    output_hub::Subscription raw;
    std::string instance;
    std::string startup;
    // Protects admission, identities, counters, queues and status; never held for disk I/O.
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint64_t, std::unique_ptr<Peer>> peers;
    std::map<std::uint64_t, std::string> partial;
    std::deque<Pending> pending;
    std::deque<std::shared_ptr<Record>> history;
    std::size_t pending_bytes = 0;
    std::size_t history_bytes = 0;
    std::uint64_t sequence = 0;
    std::uint64_t dropped = 0;
    std::uint64_t raw_lost_bytes = 0;
    std::uint64_t raw_lost_events = 0;
    std::uint64_t transport_dropped = 0;
    std::uint64_t uncertain_sessions = 0;
    std::uint64_t source_rejected = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t file_lost = 0;
    std::optional<Error> failure;
    std::uint64_t revision = 0;
    std::filesystem::path root;
    FileOptions options;
    bool reconfigure = false;
    bool configured = false;
    bool enabled = false;
    bool closing = false;
    bool writer_failed = false;
    std::jthread collector;
    std::jthread writer;

    Impl(control::Service& channels, output_hub::Subscription subscription, std::string name)
        : service(channels), raw(std::move(subscription)), instance(std::move(name)) {
        startup = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void fail(Error error) {
        failure = error;
        ++revision;
    }

    Record record(std::string name, std::uint64_t mid, std::uint64_t pid) {
        Record value;
        value.instance = instance;
        value.startup = startup;
        value.module = std::move(name);
        value.mid = mid;
        value.pid = pid;
        value.received = std::chrono::system_clock::now();
        value.monotonic = std::chrono::steady_clock::now();
        return value;
    }

    void enqueue(Record value) {
        value.sequence = ++sequence;
        auto item = std::make_shared<Record>(std::move(value));
        // Account all variable text plus a conservative fixed object/metadata budget.
        const auto bytes = item->text.size() + item->module.size() + item->instance.size() +
                           item->startup.size() + sizeof(Record);
        const bool urgent = item->level && *item->level <= Level::Warn;
        const auto records_limit = config::log_queue_records - (urgent ? 0 : config::log_reserved_records);
        const auto bytes_limit = config::log_queue_bytes - (urgent ? 0 : config::log_reserved_bytes);
        if (pending.size() >= records_limit || pending_bytes > bytes_limit ||
            bytes > bytes_limit - pending_bytes) {
            ++dropped;
            if (auto peer = peers.find(item->mid); peer != peers.end()) {
                ++peer->second->disk_dropped;
            }
            ++revision;
        } else {
            pending.push_back({item, bytes});
            pending_bytes += bytes;
            changed.notify_one();
        }
        // Shell is a separate lossy observer; it cannot retain or block the disk queue.
        while (!history.empty() && (history.size() >= config::log_queue_records ||
                                    history_bytes + bytes > config::log_queue_bytes)) {
            const auto& old = history.front();
            history_bytes -= old->text.size() + old->module.size() + old->instance.size() +
                             old->startup.size() + sizeof(Record);
            history.pop_front();
        }
        history.push_back(std::move(item));
        history_bytes += bytes;
    }

    void diagnostic(std::string text, Level level) {
        auto value = record("vinc", 0, 0);
        value.origin = Origin::Diagnostic;
        value.level = level;
        value.text = std::move(text);
        enqueue(std::move(value));
    }

    void collect_raw() {
        auto batch = raw.take(config::log_collection_budget);
        if (batch.skipped.events) {
            raw_lost_events += batch.skipped.events;
            raw_lost_bytes += batch.skipped.bytes;
            partial.clear();
            for (const auto& lost : batch.sources_skipped) {
                if (auto peer = peers.find(lost.source->generation); peer != peers.end()) {
                    peer->second->raw_dropped_bytes += lost.loss.bytes;
                    peer->second->raw_dropped_chunks += lost.loss.events;
                }
                diagnostic(
                    fmt::format("OutputHub source gap: module={} mid={} pid={} stream={} chunks={} bytes={}",
                                lost.source->name, lost.source->generation, lost.source->pid,
                                lost.source->stream == output_hub::Stream::Stdout ? "stdout" : "stderr",
                                lost.loss.events, lost.loss.bytes),
                    Level::Warn);
            }
            diagnostic(fmt::format("OutputHub gap: {} chunks, {} bytes; unfinished lines discarded",
                                   batch.skipped.events, batch.skipped.bytes),
                       Level::Warn);
        }
        for (const auto& event : batch.events) {
            const auto& source = *event->source;
            auto value = record(source.name, source.generation, source.pid);
            if (auto peer = peers.find(source.generation); peer != peers.end()) {
                value.pid = peer->second->pid;
            }
            value.origin = source.stream == output_hub::Stream::Stdout ? Origin::Stdout : Origin::Stderr;
            value.monotonic = event->received;
            value.received = event->wall_received;
            auto& text = partial[source.id];
            if (event->kind == output_hub::EventKind::Data) {
                for (char byte : event->bytes) {
                    if (byte == '\n') {
                        value.text = std::move(text);
                        text.clear();
                        enqueue(value);
                    } else {
                        text += byte;
                        if (text.size() >= config::log_message_bytes) {
                            auto boundary = text.size() - 1;
                            while (boundary && (static_cast<unsigned char>(text[boundary]) & 0xc0) == 0x80) {
                                --boundary;
                            }
                            if (static_cast<unsigned char>(text[boundary]) < 0x80) {
                                boundary = text.size();
                            }
                            if (!boundary) {
                                boundary = text.size();
                            }
                            value.text = "[fragment] " + text.substr(0, boundary);
                            text.erase(0, boundary);
                            enqueue(value);
                        }
                    }
                }
            } else {
                if (!text.empty()) {
                    value.text = "[tail] " + std::move(text);
                    enqueue(value);
                }
                partial.erase(source.id);
                if (event->kind != output_hub::EventKind::End &&
                    event->kind != output_hub::EventKind::Renamed) {
                    diagnostic("OutputHub stream " + std::to_string(source.id) +
                                   " ended with truncation/failure",
                               Level::Warn);
                }
            }
        }
    }

    void collect_peer(Peer& peer) {
        if (peer.listener && !peer.session) {
            auto accepted = peer.listener->try_accept();
            if (accepted) {
                auto attached = service.attach(std::move(*accepted), peer.options);
                if (attached) {
                    peer.session = std::move(*attached);
                } else {
                    fail(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, attached.error()));
                }
                peer.listener.reset();
            } else if (!accepted.error().is(osal::ErrorCode::WouldBlock)) {
                fail(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, accepted.error()));
                peer.listener.reset();
            }
        }
        if (!peer.session) {
            return;
        }
        if (peer.level_pending && peer.session->state() == control::State::Open) {
            control::Frame level{control::Kind::Event, domain, level_operation};
            control::put_u32(level.payload, protocol_version);
            control::put_u32(level.payload, static_cast<std::uint32_t>(peer.level));
            if (peer.session->send(std::move(level))) {
                peer.level_pending = false;
            }
        }
        const auto losses = peer.session->queue_stats();
        if (losses.dropped_frames != peer.dropped) {
            transport_dropped += losses.dropped_frames - peer.dropped;
            peer.dropped = losses.dropped_frames;
            diagnostic(fmt::format("Log receive gap: module={} mid={} dropped_frames={}", peer.name, peer.mid,
                                   peer.dropped),
                       Level::Warn);
        }
        for (std::size_t i = 0; i < config::log_collection_budget; ++i) {
            auto frame = peer.session->try_receive();
            if (!frame) {
                if (!peer.fenced && !peer.disconnect_reported) {
                    peer.disconnect_reported = true;
                    ++uncertain_sessions;
                    fail(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Receive, frame.error()));
                    diagnostic(
                        fmt::format("Log disconnected: module={} mid={}; final in-flight delivery unknown",
                                    peer.name, peer.mid),
                        Level::Warn);
                }
                break;
            }
            if (!*frame) {
                break;
            }
            const auto& wire = **frame;
            if (wire.kind == control::Kind::Event && wire.domain == domain &&
                wire.operation == statistics_operation && wire.request == 0) {
                control::Reader reader(wire.payload);
                const auto version = reader.u32();
                const auto rejected = reader.u64();
                if (version != protocol_version || rejected < peer.rejected || !reader.done()) {
                    peer.session->close();
                    fail({ErrorCode::Protocol, ErrorOperation::Receive});
                    break;
                }
                source_rejected += rejected - peer.rejected;
                peer.rejected = rejected;
                continue;
            }
            if (wire.kind == control::Kind::Request && wire.domain == domain &&
                wire.operation == fence_operation && wire.request == 1) {
                control::Reader reader(wire.payload);
                const auto version = reader.u32();
                const auto last = reader.u64();
                const auto rejected = reader.u64();
                if (version != protocol_version || !reader.done() || last < peer.last_sequence ||
                    rejected < peer.rejected) {
                    peer.session->close();
                    fail({ErrorCode::Protocol, ErrorOperation::Receive});
                    break;
                }
                source_rejected += rejected - peer.rejected;
                peer.rejected = rejected;
                if (last != peer.last_sequence) {
                    diagnostic(fmt::format("Log final sequence gap: module={} mid={} after={} final={}",
                                           peer.name, peer.mid, peer.last_sequence, last),
                               Level::Warn);
                }
                peer.fenced = true;
                (void)peer.session->send({control::Kind::Response, domain, fence_operation, 1});
                continue;
            }
            auto message = decode_message(wire);
            if (!message || message->sequence <= peer.last_sequence || message->rejected < peer.rejected) {
                peer.session->close();
                fail({ErrorCode::Protocol, ErrorOperation::Receive});
                break;
            }
            if (message->sequence != peer.last_sequence + 1) {
                diagnostic(fmt::format("Log sequence gap: module={} mid={} after={} next={}", peer.name,
                                       peer.mid, peer.last_sequence, message->sequence),
                           Level::Warn);
            }
            peer.last_sequence = message->sequence;
            source_rejected += message->rejected - peer.rejected;
            peer.rejected = message->rejected;
            auto value = record(peer.name, peer.mid, peer.pid);
            value.level = message->level;
            value.produced_us = message->timestamp_us;
            value.text = std::move(message->text);
            enqueue(std::move(value));
        }
    }

    void collect() {
        std::lock_guard lock(mutex);
        collect_raw();
        for (auto it = peers.begin(); it != peers.end();) {
            auto& peer = *it->second;
            collect_peer(peer);
            if (peer.exited && (!peer.session || peer.session->state() == control::State::Closed)) {
                // A closed session may still contain frames; drain it over subsequent turns.
                // A generous final budget is bounded by the transport queue capacity.
                for (std::size_t i = 0; i < config::log_collection_budget; ++i) {
                    collect_peer(peer);
                }
                if (!peer.fenced && !peer.disconnect_reported) {
                    ++uncertain_sessions;
                    diagnostic(
                        fmt::format("Log disconnected: module={} mid={}; final in-flight delivery unknown",
                                    peer.name, peer.mid),
                        Level::Warn);
                }
                it = peers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void start_workers() {
        auto* state = this;
        writer = std::jthread([state] {
            try {
                state->write_loop();
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->enabled = false;
                state->configured = true;
                state->writer_failed = true;
                state->fail({ErrorCode::ResourceExhausted, ErrorOperation::Write});
                state->changed.notify_all();
            }
        });
        collector = std::jthread([state](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    state->collect();
                    std::this_thread::sleep_for(config::log_poll_interval);
                }
                state->collect();
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->fail({ErrorCode::ResourceExhausted, ErrorOperation::Receive});
            }
        });
    }

    void write_loop() {
        Files files;
        auto flush_at = std::chrono::steady_clock::now() + config::log_flush_interval;
        std::uint64_t last_written = 0;
        for (;;) {
            std::unique_lock lock(mutex);
            changed.wait_until(lock, flush_at, [&] {
                return closing || reconfigure || !pending.empty();
            });
            if (reconfigure) {
                auto path = root;
                auto limits = options;
                reconfigure = false;
                lock.unlock();
                auto configured_result = files.configure(path, limits);
                lock.lock();
                configured = true;
                enabled = bool(configured_result);
                if (!configured_result) {
                    fail(configured_result.error());
                } else {
                    failure.reset();
                    ++revision;
                }
                file_bytes = files.bytes();
                changed.notify_all();
            }
            if (!pending.empty()) {
                auto item = std::move(pending.front());
                pending.pop_front();
                pending_bytes -= item.bytes;
                const bool write_enabled = enabled;
                lock.unlock();
                Result<void> result;
                if (write_enabled) {
                    auto encoded = render(*item.record);
                    if (last_written && item.record->sequence != last_written + 1) {
                        encoded = "[disk gap after seq=" + std::to_string(last_written) + "] " + encoded;
                    }
                    result = files.append(date_folder(item.record->received), encoded);
                    if (result) {
                        last_written = item.record->sequence;
                    }
                    if (result && item.record->level == Level::Error) {
                        result = files.flush();
                    }
                }
                lock.lock();
                if (!write_enabled || !result) {
                    ++file_lost;
                }
                if (!result) {
                    enabled = false;
                    fail(result.error());
                }
                file_bytes = files.bytes();
            }
            if (std::chrono::steady_clock::now() >= flush_at || (closing && pending.empty())) {
                lock.unlock();
                auto flushed = files.flush();
                lock.lock();
                if (!flushed) {
                    enabled = false;
                    fail(flushed.error());
                }
                flush_at = std::chrono::steady_clock::now() + config::log_flush_interval;
            }
            if (closing && pending.empty()) {
                return;
            }
        }
    }
};

Manager::Manager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

Result<std::unique_ptr<Manager>> Manager::create(control::Service& service, output_hub::OutputHub& output,
                                                 std::string instance) try {
    auto subscription = output.subscribe();
    if (!subscription) {
        return std::unexpected(
            wrap_error(ErrorCode::ResourceExhausted, ErrorOperation::Create, subscription.error()));
    }
    auto manager = std::unique_ptr<Manager>(
        new Manager(std::make_unique<Impl>(service, std::move(*subscription), std::move(instance))));
    auto* state = manager->impl_.get();
    state->start_workers();
    return manager;
} catch (const std::system_error& error) {
    return std::unexpected(
        Error{ErrorCode::ResourceExhausted, ErrorOperation::Create, osal::native_error(error.code())});
} catch (const std::bad_alloc&) {
    return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Create});
}

Manager::~Manager() {
    impl_->collector.request_stop();
    if (impl_->collector.joinable()) {
        impl_->collector.join();
    }
    // OutputHub must have drained first. Drain its bounded queue before stopping disk ownership.
    try {
        for (std::size_t i = 0;
             i < config::output_hub_default_queue_events / config::log_collection_budget + 1; ++i) {
            impl_->collect();
        }
    } catch (...) {
        // Shutdown still joins disk ownership after exhausted diagnostic storage.
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closing = true;
    }
    impl_->changed.notify_all();
    if (impl_->writer.joinable()) {
        impl_->writer.join();
    }
    if (impl_->failure) {
        // The Shell may already be gone. This central fallback is not captured by Log.
        const auto& error = *impl_->failure;
        std::fprintf(stderr, "Log shutdown diagnostic: domain=%u code=%d operation=%d native=%lld\n",
                     static_cast<unsigned>(error.domain), error.code, error.operation,
                     static_cast<long long>(error.native.code));
    }
}

Result<void> Manager::configure(std::filesystem::path root, FileOptions options) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->writer_failed) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Configure});
    }
    impl_->root = std::move(root);
    impl_->options = options;
    impl_->configured = false;
    impl_->reconfigure = true;
    impl_->changed.notify_all();
    impl_->changed.wait(lock, [&] {
        return impl_->configured;
    });
    if (impl_->failure) {
        return std::unexpected(*impl_->failure);
    }
    return {};
}

Result<void> Manager::listen(std::uint64_t mid, std::string endpoint, control::Options options) {
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, socket.error()));
    }
    auto peer = std::make_unique<Impl::Peer>();
    peer->mid = mid;
    peer->name = "starting:" + std::to_string(mid);
    peer->endpoint = std::move(endpoint) + ".log";
    auto bound = socket->bind(osal::LocalEndpoint{peer->endpoint});
    if (!bound) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, bound.error()));
    }
    auto listening = socket->listen(1);
    if (!listening) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, listening.error()));
    }
    options.token += ":log";
    options.receive_overflow = control::ReceiveOverflow::DropNewest;
    peer->options = std::move(options);
    peer->listener = std::move(*socket);
    std::lock_guard lock(impl_->mutex);
    if (impl_->peers.size() >= control::detail::default_session_capacity || impl_->peers.contains(mid)) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Connect});
    }
    impl_->peers.emplace(mid, std::move(peer));
    return {};
}

void Manager::identify(std::uint64_t mid, std::string name, std::uint64_t pid) {
    std::lock_guard lock(impl_->mutex);
    if (auto it = impl_->peers.find(mid); it != impl_->peers.end()) {
        it->second->name = std::move(name);
        it->second->pid = pid;
    }
}

void Manager::exited(std::uint64_t mid) {
    std::lock_guard lock(impl_->mutex);
    if (auto it = impl_->peers.find(mid); it != impl_->peers.end()) {
        it->second->exited = true;
    }
}

void Manager::diagnostic(std::string text, Level level) {
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->diagnostic(std::move(text), level);
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        impl_->fail({ErrorCode::ResourceExhausted, ErrorOperation::Write});
    }
}

Result<std::string> Manager::command(const std::vector<std::string>& args) {
    std::lock_guard lock(impl_->mutex);
    if (args == std::vector<std::string>{"status"}) {
        std::string status = fmt::format(
            "Log file={} bytes={} queued={} dropped={} raw_lost_chunks={} raw_lost_bytes={} "
            "transport_dropped={} source_rejected={} uncertain_sessions={} file_lost={}\n",
            impl_->enabled ? "enabled" : "disabled", impl_->file_bytes, impl_->pending.size(), impl_->dropped,
            impl_->raw_lost_events, impl_->raw_lost_bytes, impl_->transport_dropped, impl_->source_rejected,
            impl_->uncertain_sessions, impl_->file_lost);
        if (impl_->failure) {
            status += error_to_string(*impl_->failure);
        }
        for (const auto& [mid, peer] : impl_->peers) {
            status += fmt::format("\n{} mid={} pid={} source_level={} source_rejected={} receive_dropped={} "
                                  "disk_dropped={} raw_dropped_chunks={} raw_dropped_bytes={}",
                                  peer->name, mid, peer->pid, static_cast<unsigned>(peer->level),
                                  peer->rejected, peer->dropped, peer->disk_dropped, peer->raw_dropped_chunks,
                                  peer->raw_dropped_bytes);
        }
        return status;
    }
    if (args == std::vector<std::string>{"reopen"}) {
        if (impl_->writer_failed || impl_->root.empty()) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Configure});
        }
        impl_->reconfigure = true;
        impl_->changed.notify_all();
        return "Log reopen queued; inspect log status";
    }
    if (args.size() == 3 && args[0] == "level") {
        const auto level = parse_level(args[2]);
        if (level) {
            bool found = false;
            for (auto& [mid, peer] : impl_->peers) {
                if (args[1] == "all" || args[1] == peer->name || args[1] == std::to_string(mid)) {
                    peer->level = *level;
                    peer->level_pending = true;
                    found = true;
                }
            }
            if (found) {
                return "Log source level update queued";
            }
        }
    }
    return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
}

std::string Manager::take(View& view) {
    std::lock_guard lock(impl_->mutex);
    if (!view.enabled) {
        view.cursor = impl_->sequence;
        return {};
    }
    std::string text;
    if (!impl_->history.empty() && view.cursor + 1 < impl_->history.front()->sequence) {
        text += "[Log display gap]\n";
    }
    std::size_t count = 0;
    for (const auto& item : impl_->history) {
        if (item->sequence <= view.cursor) {
            continue;
        }
        // Raw output remains exclusively controlled by the existing output command.
        if (item->origin != Origin::Stdout && item->origin != Origin::Stderr &&
            (!item->level || *item->level <= view.level) &&
            (view.module.empty() || view.module == item->module ||
             view.module == std::to_string(item->mid))) {
            auto line = render(*item) + "\n";
            if (text.size() + line.size() > config::log_display_bytes) {
                break;
            }
            text += line;
            view.cursor = item->sequence;
            if (++count == config::log_collection_budget) {
                break;
            }
        }
        view.cursor = item->sequence;
    }
    return text;
}

std::string Manager::notification(std::uint64_t& cursor) {
    std::lock_guard lock(impl_->mutex);
    if (cursor == impl_->revision) {
        return {};
    }
    cursor = impl_->revision;
    if (impl_->failure) {
        return "Log diagnostic: " + error_to_string(*impl_->failure);
    }
    if (impl_->dropped) {
        return fmt::format("Log queue dropped {} records; use log status", impl_->dropped);
    }
    return {};
}

Result<std::string> view_command(View& view, const std::vector<std::string>& args) {
    if (args.size() == 3 && args[0] == "log" && args[1] == "show") {
        if (args[2] == "on" || args[2] == "off") {
            view.enabled = args[2] == "on";
            return view.enabled ? "Log display on" : "Log display off";
        }
        if (auto level = parse_level(args[2])) {
            view.level = *level;
            return "Log display level updated";
        }
    }
    if (args.size() == 4 && args[1] == "show" && args[2] == "module") {
        view.module = args[3] == "all" ? "" : args[3];
        return "Log display module filter updated";
    }
    return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Command});
}
}
