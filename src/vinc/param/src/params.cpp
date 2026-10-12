#include <vinc/param/params.hpp>
#include <vinc/param/detail/operation_error.hpp>

namespace vinc::param {
Result<void> Params::watch(std::string name, Type type) {
    if (!access_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Watch});
    }

    auto full = qualify(access_->module_name(), name);
    if (!full) {
        return std::unexpected(detail::operation_error(ErrorOperation::Watch, full.error()));
    }
    {
        std::lock_guard lock(mutex_);
        if (auto found = watched_.find(*full); found != watched_.end()) {
            if (found->second != type) {
                return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Watch});
            }
        } else {
            if (watched_.size() == config::param_entry_limit || watch_revision_ >= UINT64_MAX - 1) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Watch});
            }

            watched_.emplace(*full, type);
            ++watch_revision_;
        }
    }
    auto result = access_->watch_value(std::move(*full), type);
    if (!result) {
        return std::unexpected(detail::operation_error(ErrorOperation::Watch, result.error()));
    }
    return result;
}

Result<void> Params::register_value(std::string name, Value defaults, Permission permission) {
    if (!access_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Register});
    }
    if (!valid_segment(name)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }

    auto valid = validate_value(defaults);
    if (!valid) {
        return std::unexpected(detail::operation_error(ErrorOperation::Register, valid.error()));
    }
    // Reserve the watch before the remote mutation. On failure it remains a
    // visible missing watch rather than losing ownership of a successful registration.
    auto watched = watch(name, type_of(defaults));
    if (!watched) {
        return std::unexpected(detail::operation_error(ErrorOperation::Register, watched.error()));
    }

    auto result = access_->register_value(std::move(name), std::move(defaults), permission);
    if (!result) {
        return std::unexpected(detail::operation_error(ErrorOperation::Register, result.error()));
    }
    return result;
}

Result<void> Params::set_value(std::string name, Value value) {
    if (!access_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Set});
    }

    auto full = qualify(access_->module_name(), name);
    if (!full) {
        return std::unexpected(detail::operation_error(ErrorOperation::Set, full.error()));
    }

    auto valid = validate_value(value);
    if (!valid) {
        return std::unexpected(detail::operation_error(ErrorOperation::Set, valid.error()));
    }

    auto result = access_->set_value(std::move(*full), std::move(value));
    if (!result) {
        return std::unexpected(detail::operation_error(ErrorOperation::Set, result.error()));
    }
    return result;
}

Result<Value> Params::read(std::string_view name) const {
    if (!access_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Update});
    }

    auto full = qualify(access_->module_name(), name);
    if (!full) {
        return std::unexpected(detail::operation_error(ErrorOperation::Update, full.error()));
    }
    std::lock_guard lock(mutex_);
    const auto found = cache_.find(*full);
    if (found == cache_.end()) {
        return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Update});
    }

    return found->second.value;
}

Result<void> Params::on_change(std::function<void(const ParamMap&)> callback) {
    auto replacement =
        callback ? std::make_shared<std::function<void(const ParamMap&)>>(std::move(callback)) : nullptr;
    // Destroy old user captures outside the registry lock.
    {
        std::lock_guard lock(mutex_);
        if (watch_revision_ >= UINT64_MAX - 1) {
            return std::unexpected(Error{ErrorCode::VersionExhausted, ErrorOperation::Watch});
        }

        callback_.swap(replacement);
        ++watch_revision_;
    }
    return {};
}

std::vector<WatchStatus> Params::watches() const {
    std::lock_guard lock(mutex_);
    std::vector<WatchStatus> result;
    for (const auto& [name, type] : watched_) {
        const auto found = cache_.find(name);
        result.push_back({name, type, found != cache_.end(),
                          found != cache_.end() && type_of(found->second.value) == type});
    }
    return result;
}

Result<std::uint64_t> Params::pump() {
    if (!access_) {
        return std::uint64_t{0};
    }
    std::map<std::string, Type, std::less<>> watched;
    std::uint64_t watch_revision;
    std::uint64_t known;
    std::shared_ptr<std::function<void(const ParamMap&)>> callback;
    {
        std::lock_guard lock(mutex_);
        if (watched_.empty()) {
            return std::uint64_t{0};
        }
        watched = watched_;
        watch_revision = watch_revision_;
        known = applied_watch_revision_ == watch_revision_ ? revision_ : UINT64_MAX;
        callback = callback_;
    }
    auto snapshot = access_->snapshot(known);
    if (!snapshot) {
        return std::unexpected(detail::operation_error(ErrorOperation::Pump, snapshot.error()));
    }
    if (!*snapshot) {
        return std::uint64_t{0};
    }
    ParamMap next;
    for (const auto& [name, type] : watched) {
        auto found = (*snapshot)->entries.find(name);
        if (found != (*snapshot)->entries.end()) {
            next.emplace(name, std::move(found->second));
        }
    }
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = next.size() != cache_.size() || applied_watch_revision_ != watch_revision;
        for (const auto& [name, entry] : next) {
            const auto old = cache_.find(name);
            if (old == cache_.end() || old->second.version != entry.version) {
                changed = true;
            }
        }
        cache_.swap(next);
        revision_ = (*snapshot)->revision;
        applied_watch_revision_ = watch_revision;
    }
    // Only Context's single active pump mutates cache_. Registration changes the
    // watch registry, not this borrowed map. Concurrent update() is read-only.
    if (changed && callback) {
        try {
            (*callback)(cache_);
        } catch (...) {
            return std::unexpected(Error{ErrorCode::CallbackFailed, ErrorOperation::Pump});
        }

        return std::uint64_t{1};
    }
    return std::uint64_t{0};
}
}
