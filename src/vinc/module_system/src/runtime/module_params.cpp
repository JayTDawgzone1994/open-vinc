#include <vinc/module/detail/module_params.hpp>
#include <vinc/param/detail/protocol.hpp>
#include <vinc/param/detail/operation_error.hpp>

namespace vinc::detail {
namespace {
using param::ErrorCode;
using param::ErrorOperation;

Error channel_error(const Error& cause) {
    return wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Receive, cause);
}
}

ModuleParams::ModuleParams(std::shared_ptr<control::Session> session, ModuleLocks& locks)
    : session_(std::move(session)), locks_(locks) {
}

void ModuleParams::identify(std::string name) {
    name_ = std::move(name);
}

void ModuleParams::stop() noexcept {
    stop_.request_stop();
}

Result<void> ModuleParams::receive(control::Frame frame) {
    if (frame.domain != param::detail::domain) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    if (frame.kind == control::Kind::Event && frame.operation == param::detail::Changed && !frame.request) {
        control::Reader reader(frame.payload);
        const auto revision = reader.u64();
        if (!reader.done() || !revision) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }

        return {};
    }
    std::lock_guard lock(mutex_);
    if (frame.kind != control::Kind::Response || !frame.request || frame.request >= next_request_) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    const auto found = pending_.find(frame.request);
    if (found == pending_.end()) {
        return {}; // Retired timeout/cancellation response; never replay a mutation.
    }
    if (found->second->operation != frame.operation || found->second->reply) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    auto decoded = param::detail::decode_response(frame.payload);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    found->second->reply = std::move(frame.payload);
    found->second->changed.notify_all();
    return {};
}

Result<control::Bytes> ModuleParams::request(std::uint16_t operation, control::Bytes payload) {
    std::unique_lock lock(mutex_);
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::Receive});
    }
    if (pending_.size() >= config::param_pending_limit || next_request_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Receive});
    }

    auto pending = std::make_shared<Pending>();
    pending->operation = operation;
    const auto id = next_request_++;
    pending_.emplace(id, pending);

    struct Erase {
        ModuleParams& self;
        std::uint64_t id;

        ~Erase() {
            self.pending_.erase(id);
        }
    } cleanup{*this, id};

    auto sent =
        session_->send({control::Kind::Request, param::detail::domain, operation, id, std::move(payload)});
    if (!sent) {
        return std::unexpected(channel_error(sent.error()));
    }

    const bool mutation = operation == param::detail::Register || operation == param::detail::Set;
    const auto deadline = std::chrono::steady_clock::now() + config::param_request_timeout;
    const bool ready = pending->changed.wait_until(lock, stop_.get_token(), deadline, [&] {
        return pending->reply.has_value();
    });
    if (stop_.stop_requested()) {
        const auto code = mutation ? ErrorCode::CancelledOutcomeUnknown : ErrorCode::Cancelled;
        return std::unexpected(Error{code, ErrorOperation::Receive});
    }
    if (!ready) {
        const auto code = mutation ? ErrorCode::TimeoutOutcomeUnknown : ErrorCode::Timeout;
        return std::unexpected(Error{code, ErrorOperation::Receive});
    }

    auto result = param::detail::decode_response(*pending->reply);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->failure) {
        return std::unexpected(*result->failure);
    }

    return control::Bytes(result->body.begin(), result->body.end());
}

Result<void> ModuleParams::register_value(std::string name, param::Value defaults,
                                          param::Permission permission) {
    control::Bytes payload;
    control::put_u32(payload, param::detail::version);
    auto encoded = control::put_string(payload, name);
    if (!encoded) {
        return std::unexpected(channel_error(encoded.error()));
    }

    auto value = param::detail::put_value(payload, defaults);
    if (!value) {
        return value;
    }
    control::put_u32(payload, static_cast<std::uint32_t>(permission));
    auto result = request(param::detail::Register, std::move(payload));
    if (!result) {
        return std::unexpected(param::detail::operation_error(ErrorOperation::Register, result.error()));
    }
    if (!result->empty()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Register});
    }

    return {};
}

Result<void> ModuleParams::set_value(std::string name, param::Value value) {
    control::Bytes payload;
    control::put_u32(payload, param::detail::version);
    auto named = control::put_string(payload, name);
    if (!named) {
        return std::unexpected(channel_error(named.error()));
    }

    auto encoded = param::detail::put_value(payload, value);
    if (!encoded) {
        return encoded;
    }

    auto result = request(param::detail::Set, std::move(payload));
    if (!result) {
        return std::unexpected(param::detail::operation_error(ErrorOperation::Set, result.error()));
    }
    if (!result->empty()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Set});
    }

    return {};
}

Result<void> ModuleParams::watch_value(std::string name, param::Type type) {
    control::Bytes payload;
    control::put_u32(payload, param::detail::version);
    auto named = control::put_string(payload, name);
    if (!named) {
        return std::unexpected(channel_error(named.error()));
    }
    control::put_u32(payload, static_cast<std::uint32_t>(type));
    auto result = request(param::detail::Watch, std::move(payload));
    if (!result) {
        return std::unexpected(param::detail::operation_error(ErrorOperation::Watch, result.error()));
    }
    if (!result->empty()) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Watch});
    }

    return {};
}

Result<void> ModuleParams::attach() {
    control::Bytes payload;
    control::put_u32(payload, param::detail::version);
    auto result = request(param::detail::Attach, std::move(payload));
    if (!result) {
        return std::unexpected(result.error());
    }
    control::Reader reader(*result);
    const auto version = reader.u32();
    const auto id = reader.u64();
    const auto bytes = reader.u64();
    auto name = reader.string();
    if (!reader.done() || version != param::detail::version || !id || bytes != param::detail::storage_bytes) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Attach});
    }

    buffer_.reserve(config::param_slot_bytes);
    auto mapped = map_image(name);
    if (!mapped) {
        return mapped;
    }
    lock_id_ = id;
    return {};
}

Result<void> ModuleParams::map_image(std::string_view name) {
    auto memory = osal::SharedMemory::open(name, osal::SharedMemoryAccess::ReadOnly);
    if (!memory) {
        return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Attach, memory.error()));
    }

    auto mapping = memory->map(param::detail::storage_bytes, osal::SharedMemoryAccess::ReadOnly);
    if (!mapping) {
        return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Attach, mapping.error()));
    }
    mapping_ = std::move(*mapping);
    return {};
}

Result<std::optional<param::Snapshot>> ModuleParams::snapshot(std::uint64_t known_revision) {
    std::lock_guard local_lock(read_mutex_);
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::Pump});
    }
    if (!mapping_) {
        auto opened = attach();
        if (!opened) {
            return std::unexpected(opened.error());
        }
    }
    auto lease = locks_.lock(lock_id_, std::chrono::steady_clock::now() + config::param_lock_timeout);
    if (!lease) {
        return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Pump, lease.error()));
    }

    auto copied =
        param::detail::copy_image(mapping_.bytes(), (*lease)->publication(), known_revision, buffer_);
    auto released = (*lease)->unlock();
    if (!released) {
        return std::unexpected(wrap_error(ErrorCode::StorageFailed, ErrorOperation::Pump, released.error()));
    }
    if (!copied) {
        return std::unexpected(copied.error());
    }
    if (buffer_.empty()) {
        return std::optional<param::Snapshot>{};
    }

    auto snapshot = param::detail::decode_snapshot(buffer_);
    if (!snapshot || snapshot->revision != *copied) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Pump});
    }

    return std::optional<param::Snapshot>(std::move(*snapshot));
}
}
