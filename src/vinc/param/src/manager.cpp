#include <vinc/param/detail/manager.hpp>
#include <vinc/param/detail/operation_error.hpp>
#include <vinc/owner_id.hpp>
#include <sstream>
#include <cmath>

namespace vinc::param::detail {
namespace {
bool same_value(const Value& left, const Value& right) {
    if (left.index() != right.index()) {
        return false;
    }
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const auto& other = std::get<T>(right);
            if constexpr (std::is_floating_point_v<T>) {
                // A persisted NaN default is not an override merely because NaN != NaN.
                return value == other || (std::isnan(value) && std::isnan(other));
            } else {
                return value == other;
            }
        },
        left);
}

std::string display_value(const Value& value) {
    const auto* text = std::get_if<std::string>(&value);
    if (!text) {
        return value_text(value);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string output = "\"";
    for (const unsigned char byte : *text) {
        if (byte < 32 || byte == 127) {
            output += "\\x";
            output += hex[byte >> 4];
            output += hex[byte & 15];
        } else {
            if (byte == '\\' || byte == '\"') {
                output += '\\';
            }
            output += static_cast<char>(byte);
        }
    }
    output += '\"';
    return output;
}
}

Result<std::unique_ptr<ParamManager>> ParamManager::create(std::unique_ptr<Storage> storage) {
    auto result = std::make_unique<ParamManager>();
    result->storage_ = std::move(storage);
    result->worker_ = std::jthread([state = result.get()](std::stop_token stop) {
        state->run(stop);
    });
    return result;
}

ParamManager::~ParamManager() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ParamManager::run(std::stop_token stop) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] {
                return stop.stop_requested() || !queue_.empty();
            });
            if (queue_.empty()) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}

Result<void> ParamManager::enqueue(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    if (closing_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
    }
    if (queue_.size() >= config::param_queue_limit) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Receive});
    }

    queue_.push_back(std::move(work));
    changed_.notify_one();
    return {};
}

void ParamManager::admit(std::uint64_t mid, std::string name) {
    std::lock_guard lock(mutex_);
    peers_.try_emplace(mid, Peer{std::move(name), {}, {}});
}

void ParamManager::exited(std::uint64_t mid) {
    std::lock_guard lock(mutex_);
    peers_.erase(mid);
}

Result<void> ParamManager::submit(std::uint64_t mid, std::shared_ptr<control::Session> session,
                                  control::Frame frame) {
    if (frame.kind != control::Kind::Request || frame.domain != domain || !frame.request ||
        frame.payload.size() > request_max_bytes) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    {
        std::lock_guard lock(mutex_);
        const auto peer = peers_.find(mid);
        if (peer == peers_.end()) {
            return std::unexpected(Error{ErrorCode::PermissionDenied, ErrorOperation::Receive});
        }
        if (frame.request <= peer->second.last_request) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
        }

        peer->second.last_request = frame.request;
        peer->second.session = session;
    }
    return enqueue([this, mid, session = std::move(session), frame = std::move(frame)] {
        Result<control::Bytes> result;
        try {
            result = execute(mid, frame);
            if (!result) {
                auto operation = ErrorOperation::Receive;
                switch (frame.operation) {
                case Register:
                    operation = ErrorOperation::Register;
                    break;
                case Set:
                    operation = ErrorOperation::Set;
                    break;
                case Watch:
                    operation = ErrorOperation::Watch;
                    break;
                case Attach:
                    operation = ErrorOperation::Attach;
                    break;
                default:
                    break;
                }
                result = std::unexpected(operation_error(operation, result.error()));
            }
        } catch (const std::bad_alloc&) {
            result = std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Receive});
        } catch (const std::exception&) {
            result = std::unexpected(Error{ErrorCode::StorageFailed, ErrorOperation::Receive});
        }
        try {
            control::Frame reply{control::Kind::Response, domain, frame.operation, frame.request};
            if (result) {
                control::put_u32(reply.payload, result_success);
                reply.payload.insert(reply.payload.end(), result->begin(), result->end());
            } else {
                reply.payload = encode_failure(result.error());
            }
            if (!session->send(std::move(reply))) {
                session->close();
            }
        } catch (...) {
            // A committed request may have an unknown outcome to its caller.
            // Closing the transport wakes it; never replay a mutation automatically.
            session->close();
        }
    });
}

Result<control::Bytes> ParamManager::execute(std::uint64_t mid, const control::Frame& request) {
    std::string module;
    {
        std::lock_guard lock(mutex_);
        const auto found = peers_.find(mid);
        if (found == peers_.end()) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
        }
        module = found->second.name;
    }
    control::Reader reader(request.payload);
    if (reader.u32() != version) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    control::Bytes body;
    if (request.operation == Attach) {
        if (!reader.done()) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Attach});
        }
        control::put_u32(body, version);
        control::put_u64(body, storage_->descriptor().lock_id);
        control::put_u64(body, storage_bytes);
        auto encoded = control::put_string(body, storage_->descriptor().data_name);
        if (!encoded) {
            return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Attach, encoded.error()));
        }

        return body;
    }
    auto name = reader.string();
    if (request.operation == Watch) {
        const auto type = static_cast<Type>(reader.u32());
        if (!reader.done() || !valid_name(name) || type_name(type).empty()) {
            return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Watch});
        }
        std::lock_guard lock(mutex_);
        const auto found = peers_.find(mid);
        if (found == peers_.end()) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Watch});
        }
        if (found->second.watched.size() >= config::param_entry_limit &&
            !found->second.watched.contains(name)) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Watch});
        }

        found->second.watched.insert(std::move(name));
        return body;
    }

    auto value = read_value(reader);
    if (!value) {
        return std::unexpected(value.error());
    }
    Result<void> changed;
    if (request.operation == Register) {
        const auto permission = reader.u32();
        if (!reader.done() || permission > 1 || !valid_segment(name)) {
            return std::unexpected(Error{ErrorCode::InvalidValue, ErrorOperation::Register});
        }

        auto full = qualify(module, name);
        if (!full) {
            return std::unexpected(full.error());
        }
        changed =
            register_value(mid, std::move(*full), std::move(*value), static_cast<Permission>(permission));
    } else if (request.operation == Set && reader.done()) {
        changed = set_value(mid, name, std::move(*value));
    } else {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }
    if (!changed) {
        return std::unexpected(changed.error());
    }

    return body;
}

Result<void> ParamManager::commit(Snapshot next, const std::string& name) {
    // UINT64_MAX is reserved for a client that has no cached revision.
    if (live_.revision >= UINT64_MAX - 1) {
        return std::unexpected(Error{ErrorCode::VersionExhausted, ErrorOperation::Set});
    }

    next.revision = live_.revision + 1;
    auto& entry = next.entries.find(name)->second;
    entry.version = next.revision;
    entry.timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());

    struct Notification {
        std::shared_ptr<control::Session> session;
        control::Frame frame;
    };

    std::vector<Notification> notices;
    {
        std::lock_guard lock(mutex_);
        for (auto& [mid, peer] : peers_) {
            if (peer.watched.contains(name)) {
                if (auto session = peer.session.lock()) {
                    control::Frame frame{control::Kind::Event, domain, Changed};
                    control::put_u64(frame.payload, next.revision);
                    notices.push_back({std::move(session), std::move(frame)});
                }
            }
        }
    }
    auto written = storage_->publish(next, worker_.get_stop_token());
    if (!written) {
        return written;
    }

    live_.entries.swap(next.entries);
    live_.revision = next.revision;
    for (auto& notice : notices) {
        try {
            // Advisory only: shared revision checks repair delayed/dropped notices.
            (void)notice.session->send(std::move(notice.frame));
        } catch (...) {
            // No allocation or transport failure can undo a published image.
        }
    }
    return {};
}

void ParamManager::report(std::optional<Error> error, std::string message) {
    std::lock_guard lock(diagnostic_mutex_);
    diagnostic_error_ = error;
    diagnostic_message_.swap(message);
    // This is a bounded latest-diagnostic slot, not a growing log queue.
    if (diagnostic_revision_ != UINT64_MAX) {
        ++diagnostic_revision_;
    }
}

std::string ParamManager::diagnostic(std::uint64_t& cursor) {
    std::optional<Error> error;
    std::string message;
    {
        std::lock_guard lock(diagnostic_mutex_);
        if (cursor == diagnostic_revision_) {
            return {};
        }
        message = diagnostic_message_;
        error = diagnostic_error_;
        cursor = diagnostic_revision_;
    }
    if (error) {
        message += " " + error_to_string(*error);
    }
    return message.empty() ? message : "[Param] " + message;
}

void ParamManager::persist_defaults() {
    if (persistence_.enabled()) {
        try {
            auto result = persistence_.write(saved_);
            if (!result) {
                file_failure_ = result.error();
            }
        } catch (const std::bad_alloc&) {
            file_failure_ = Error{ErrorCode::ResourceExhausted, ErrorOperation::Save};
        } catch (const std::exception&) {
            file_failure_ = Error{ErrorCode::FileFailed, ErrorOperation::Save};
        }
        if (file_failure_) {
            report(file_failure_);
        }
    }
}

Result<void> ParamManager::register_value(std::uint64_t mid, std::string name, Value defaults,
                                          Permission permission) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }

    auto valid = validate_value(defaults);
    if (!valid) {
        return valid;
    }
    if (auto owner = registered_.find(name); owner != registered_.end() && owner->second == mid) {
        return std::unexpected(Error{ErrorCode::AlreadyRegistered, ErrorOperation::Register});
    }
    if (!live_.entries.contains(name) && live_.entries.size() >= config::param_entry_limit) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Register});
    }

    auto saved = saved_;
    auto next = live_;
    auto previous = saved.find(name);
    Value current = defaults;
    std::string type_notice;
    if (previous == saved.end()) {
        saved.emplace(name, SavedEntry{defaults, defaults});
    } else if (type_of(previous->second.default_value) != type_of(defaults)) {
        previous->second = SavedEntry{defaults, defaults};
        type_notice = "Parameter type changed; reset to default: " + name;
    } else {
        const auto old_default = previous->second.default_value;
        if (same_value(previous->second.value, old_default)) {
            previous->second.value = defaults;
        }
        previous->second.default_value = defaults;
        current = previous->second.value;
        if (auto active = live_.entries.find(name); active != live_.entries.end()) {
            current = same_value(active->second.value, old_default) ? defaults : active->second.value;
        }
    }
    next.entries.insert_or_assign(name, Entry{std::move(current), defaults, permission});
    // Prepare all registry allocations before publishing shared state.
    auto registrations = registered_;
    registrations.insert_or_assign(name, mid);
    auto reported_notice = type_notice;
    auto committed = commit(std::move(next), name);
    if (!committed) {
        return committed;
    }

    registered_.swap(registrations);
    saved_.swap(saved);
    if (!type_notice.empty()) {
        notice_.swap(type_notice);
        report({}, std::move(reported_notice));
    }
    persist_defaults();
    return {};
}

Result<void> ParamManager::set_value(std::uint64_t mid, const std::string& name, Value value) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Set});
    }

    const auto found = live_.entries.find(name);
    if (found == live_.entries.end()) {
        return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Set});
    }
    if (mid != central_owner && found->second.permission != Permission::Open) {
        return std::unexpected(Error{ErrorCode::PermissionDenied, ErrorOperation::Set});
    }
    if (type_of(value) != type_of(found->second.value)) {
        return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Set});
    }

    auto valid = validate_value(value);
    if (!valid) {
        return valid;
    }

    auto next = live_;
    next.entries.find(name)->second.value = std::move(value);
    return commit(std::move(next), name);
}

Result<void> ParamManager::load(std::filesystem::path path) {
    if (!registered_.empty() || persistence_.enabled()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::Load});
    }

    auto loaded = persistence_.load(std::move(path));
    if (!loaded) {
        file_failure_ = loaded.error();
        report(file_failure_);
        persist_defaults();
        return std::unexpected(loaded.error());
    }
    Snapshot snapshot;
    snapshot.revision = loaded->empty() ? 0 : 1;
    for (const auto& [name, entry] : *loaded) {
        snapshot.entries.emplace(name,
                                 Entry{entry.value, entry.default_value, Permission::FrameworkOnly, 1, 0});
    }
    auto written = storage_->publish(snapshot, worker_.get_stop_token());
    if (!written) {
        return written;
    }
    saved_ = std::move(*loaded);
    live_ = std::move(snapshot);
    return {};
}

Result<void> ParamManager::save() {
    auto next = saved_;
    for (const auto& [name, entry] : live_.entries) {
        next.insert_or_assign(name, SavedEntry{entry.default_value, entry.value});
    }
    auto written = persistence_.write(next);
    if (!written) {
        file_failure_ = written.error();
        report(file_failure_);
        return written;
    }

    saved_.swap(next);
    file_failure_.reset();
    report({});
    return {};
}

Result<void> ParamManager::configure(std::filesystem::path path) {
    return call(
        [this, path = std::move(path)] {
            return load(path);
        },
        true);
}

Result<void> ParamManager::register_framework(std::string name, Value defaults) {
    return call(
        [this, name = std::move(name), defaults = std::move(defaults)] {
            auto full = qualify("vinc", name);
            if (!full || !valid_segment(name)) {
                return Result<void>(std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register}));
            }

            return register_value(central_owner, *full, defaults, Permission::FrameworkOnly);
        },
        true);
}

Result<Snapshot> ParamManager::inspect() {
    return call([this]() -> Result<Snapshot> {
        return live_;
    });
}

Result<std::string> ParamManager::command(std::vector<std::string> args, std::stop_token stop) {
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::Receive});
    }

    const bool mutation = !args.empty() && (args.front() == "set" || args.front() == "save");
    return call(
        [this, args = std::move(args)] {
            return execute_command(args);
        },
        mutation, stop);
}

Result<std::string> ParamManager::execute_command(const std::vector<std::string>& args) {
    if (args.size() == 1 && args[0] == "save") {
        auto saved = save();
        if (!saved) {
            return std::unexpected(saved.error());
        }

        return std::string("Parameters saved");
    }
    if (args.size() == 1 && args[0] == "status") {
        std::string result = persistence_.enabled() ? "Persistence enabled" : "Persistence disabled";
        if (file_failure_) {
            result += "\n" + error_to_string(*file_failure_);
        }
        if (!notice_.empty()) {
            result += "\n" + notice_;
        }
        return result;
    }
    if (args.size() == 3 && args[0] == "set") {
        const auto found = live_.entries.find(args[1]);
        if (found == live_.entries.end()) {
            return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Set});
        }

        auto value = parse_value(type_of(found->second.value), args[2]);
        if (!value) {
            return std::unexpected(value.error());
        }

        auto updated = set_value(central_owner, args[1], std::move(*value));
        if (!updated) {
            return std::unexpected(updated.error());
        }

        return std::string("Parameter committed (not saved)");
    }
    const bool listing = args.size() == 1 && args[0] == "list";
    const bool getting = args.size() == 2 && args[0] == "get";
    if (!listing && !getting) {
        return std::unexpected(Error{ErrorCode::InvalidValue, ErrorOperation::Update});
    }
    std::ostringstream text;
    bool found = false;
    for (const auto& [name, entry] : live_.entries) {
        if (getting && name != args[1]) {
            continue;
        }
        found = true;
        text << name << " [" << type_name(type_of(entry.value)) << "] = " << display_value(entry.value)
             << " (default " << display_value(entry.default_value) << ", version " << entry.version << ")\n";
    }
    if (getting && !found) {
        return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Update});
    }

    return text.str();
}
}
