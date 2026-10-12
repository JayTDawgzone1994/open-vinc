#include <vinc/log/detail/transport.hpp>
#include <bit>
#include <thread>

namespace vinc::log::detail {
Result<control::Frame> encode_message(const Message& message) {
    if (message.text.size() > config::log_message_bytes || message.level > Level::Debug) {
        return std::unexpected(Error{ErrorCode::TooLarge, ErrorOperation::Encode});
    }
    control::Frame frame{control::Kind::Event, domain, record_operation};
    control::put_u32(frame.payload, protocol_version);
    control::put_u32(frame.payload, static_cast<std::uint32_t>(message.level));
    control::put_u64(frame.payload, std::bit_cast<std::uint64_t>(message.timestamp_us));
    control::put_u64(frame.payload, message.sequence);
    control::put_u64(frame.payload, message.rejected);
    auto appended = control::put_string(frame.payload, message.text);
    if (!appended) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Encode, appended.error()));
    }
    return frame;
}

Result<Message> decode_message(const control::Frame& frame) {
    control::Reader reader(frame.payload);
    const auto version = reader.u32();
    const auto level = reader.u32();
    const auto time = std::bit_cast<std::int64_t>(reader.u64());
    const auto sequence = reader.u64();
    const auto rejected = reader.u64();
    auto text = reader.string();
    if (frame.kind != control::Kind::Event || frame.domain != domain || frame.operation != record_operation ||
        frame.request != 0 || version != protocol_version ||
        level > static_cast<std::uint32_t>(Level::Debug) || sequence == 0 ||
        text.size() > config::log_message_bytes || !reader.done()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Decode});
    }
    return Message{static_cast<Level>(level), time, sequence, rejected, std::move(text)};
}

namespace {
// Socket ownership stays local until attach; each failed acquisition unwinds here.
Result<std::shared_ptr<control::Session>>
connect_session(control::Service& service, const std::string& endpoint, control::Options options) {
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, socket.error()));
    }
    const auto deadline =
        control::Clock::now() + (std::min)(options.handshake_timeout, config::log_connect_timeout);
    auto connected = socket->connect_until(osal::LocalEndpoint{endpoint + ".log"}, deadline);
    if (!connected) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, connected.error()));
    }
    options.token += ":log";
    options.reserved_send_frames = config::log_session_reserved_records;
    options.reserved_send_bytes = config::log_session_reserved_bytes;
    options.receive_overflow = control::ReceiveOverflow::DropNewest;
    auto attached = service.attach(std::move(*socket), options);
    if (!attached) {
        return std::unexpected(
            wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, attached.error()));
    }
    auto opened = (*attached)->wait_open(deadline);
    if (!opened) {
        (*attached)->close();
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Connect, opened.error()));
    }
    return std::move(*attached);
}
}

std::unique_ptr<Host> Host::connect(control::Service& service, std::string endpoint,
                                    control::Options options) {
    auto host = std::make_unique<Host>();
    auto session = connect_session(service, endpoint, std::move(options));
    if (session) {
        host->session_ = std::move(*session);
    } else {
        host->connection_failure_ = session.error();
    }
    return host;
}

bool Host::enabled(Level level) const noexcept {
    return level <= level_.load(std::memory_order_relaxed);
}

Result<void> Host::write(Level level, std::string_view text) {
    std::lock_guard lock(mutex_);
    if (!session_) {
        if (connection_failure_) {
            return std::unexpected(
                wrap_error(ErrorCode::Closed, ErrorOperation::Write, *connection_failure_));
        }
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Write});
    }
    if (sequence_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Write});
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    auto frame = encode_message({level, timestamp, ++sequence_, rejected_, std::string(text)});
    if (!frame) {
        return std::unexpected(frame.error());
    }
    auto result = session_->send(std::move(*frame), level <= Level::Warn);
    if (!result) {
        ++rejected_;
        if (result.error().is(control::ErrorCode::Backpressure)) {
            return std::unexpected(
                wrap_error(ErrorCode::Backpressure, ErrorOperation::Write, result.error()));
        }
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Write, result.error()));
    }
    return {};
}
void Host::poll() try {
    if (!session_) {
        return;
    }
    {
        // The watchdog never waits behind an application formatter or log writer.
        std::unique_lock lock(mutex_, std::try_to_lock);
        const auto now = control::Clock::now();
        if (lock.owns_lock() && rejected_ && now >= next_statistics_) {
            control::Frame statistics{control::Kind::Event, domain, statistics_operation};
            control::put_u32(statistics.payload, protocol_version);
            control::put_u64(statistics.payload, rejected_);
            (void)session_->send(std::move(statistics), true);
            next_statistics_ = now + config::log_statistics_interval;
        }
    }
    for (std::size_t i = 0; i < config::log_collection_budget; ++i) {
        auto frame = session_->try_receive();
        if (!frame || !*frame) {
            return;
        }
        control::Reader reader((**frame).payload);
        const auto version = reader.u32();
        const auto level = reader.u32();
        if ((**frame).domain != domain || (**frame).operation != level_operation ||
            (**frame).kind != control::Kind::Event || (**frame).request != 0 || version != protocol_version ||
            level > static_cast<std::uint32_t>(Level::Debug) || !reader.done()) {
            session_->close();
            return;
        }
        level_.store(static_cast<Level>(level), std::memory_order_relaxed);
    }
} catch (...) {
    if (session_) {
        session_->close();
    }
}

Host::~Host() {
    try {
        if (!session_) {
            return;
        }
        // Called after watchdog join and Plugin destruction. FIFO fence acknowledges
        // central collection, not durable storage. Exit cannot depend on log delivery.
        const auto deadline = control::Clock::now() + config::log_shutdown_timeout;
        control::Frame fence{control::Kind::Request, domain, fence_operation, 1};
        control::put_u32(fence.payload, protocol_version);
        control::put_u64(fence.payload, sequence_);
        control::put_u64(fence.payload, rejected_);
        bool sent = false;
        while (control::Clock::now() < deadline && !sent) {
            sent = bool(session_->send(fence, true));
            if (!sent) {
                std::this_thread::sleep_for(config::log_poll_interval);
            }
        }
        while (sent && control::Clock::now() < deadline) {
            auto reply = session_->receive_until(deadline);
            if (!reply) {
                break;
            }
            if (reply->kind == control::Kind::Response && reply->domain == domain &&
                reply->operation == fence_operation && reply->request == 1 && reply->payload.empty()) {
                break;
            }
        }
        session_->close();
    } catch (...) {
        // Logging is best effort; failure never prevents process teardown.
        if (session_) {
            session_->close();
        }
    }
}
}
