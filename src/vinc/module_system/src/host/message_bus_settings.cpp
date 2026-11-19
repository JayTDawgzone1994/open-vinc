#include <vinc/module/detail/message_bus_settings.hpp>
#include <vinc/module/error.hpp>

namespace vinc::detail {
namespace {
constexpr char slots_name[] = "pub_default_slots";
constexpr char bytes_name[] = "pub_default_message_bytes";
constexpr char full_slots_name[] = "vinc.pub_default_slots";
constexpr char full_bytes_name[] = "vinc.pub_default_message_bytes";
constexpr char topic_limit_name[] = "message_bus_max_topics";
constexpr char full_topic_limit_name[] = "vinc.message_bus_max_topics";

Error parameter_failure(const Error& cause) {
    return wrap_error(module_system::ErrorCode::ParamFailed,
                      module_system::ErrorOperation::AccessParam, cause);
}

Result<void> register_setting(param::detail::ParamManager& params, const char* name,
                              const char* full_name, param::Value defaults) {
    auto registered = params.register_framework(name, defaults);
    if (registered || !registered.error().is(param::ErrorCode::AlreadyRegistered)) {
        return registered;
    }

    // A previous timed-out registration may have committed. Confirm the
    // existing central declaration rather than replaying or changing it.
    auto values = params.read_current({full_name});
    if (!values) {
        return std::unexpected(values.error());
    }

    const auto& entry = values->at(full_name);
    if (entry.default_value == defaults && entry.permission == param::Permission::Limited &&
        entry.restart_hint == param::RestartHint::None) {
        return {};
    }

    return registered;
}
}

MessageBusSettings::MessageBusSettings(param::detail::ParamManager& params, message_bus::Limits limits)
    : params_(params), limits_(limits) {
}

Result<void> MessageBusSettings::initialize() {
    auto registered = register_publisher_defaults();
    if (!registered) {
        return registered;
    }

    return register_topic_limit();
}

Result<void> MessageBusSettings::register_topic_limit() {
    if (topic_limit_registered_) {
        return {};
    }

    auto result = register_setting(params_, topic_limit_name, full_topic_limit_name, limits_.max_topics);
    if (!result) {
        return std::unexpected(parameter_failure(result.error()));
    }

    topic_limit_registered_ = true;
    return {};
}

Result<std::uint32_t> MessageBusSettings::read_topic_limit() {
    auto registered = register_topic_limit();
    if (!registered) {
        return std::unexpected(registered.error());
    }

    auto values = params_.read_current({full_topic_limit_name});
    if (!values) {
        return std::unexpected(parameter_failure(values.error()));
    }

    // All uint32 values are valid. Type or lookup failures must not disable admission control.
    auto limit = values->at(full_topic_limit_name).get<std::uint32_t>();
    if (!limit) {
        return std::unexpected(parameter_failure(limit.error()));
    }

    return *limit;
}

Result<void> MessageBusSettings::register_publisher_defaults() {
    if (!slots_registered_) {
        auto registered = register_setting(params_, slots_name, full_slots_name, config::message_bus_default_slots);
        if (!registered) {
            return std::unexpected(parameter_failure(registered.error()));
        }

        slots_registered_ = true;
    }
    if (!bytes_registered_) {
        auto registered = register_setting(params_, bytes_name, full_bytes_name, config::message_bus_default_message_bytes);
        if (!registered) {
            return std::unexpected(parameter_failure(registered.error()));
        }

        bytes_registered_ = true;
    }
    return {};
}

Result<void> MessageBusSettings::restore(const param::ParamMap& values) {
    std::optional<Error> failure;
    const auto restore_one = [&](const char* full, const char* name, param::Value value) {
        if (values.at(full).value == value) {
            return;
        }

        // Ordinary central set: this may race with a later user edit. Do not
        // claim CAS semantics or implicitly persist the correction.
        auto corrected = params_.set_framework(name, std::move(value));
        if (!corrected && !failure) {
            failure = corrected.error();
        }
    };
    restore_one(full_slots_name, slots_name, effective_.slots);
    restore_one(full_bytes_name, bytes_name, effective_.max_message_bytes);
    if (failure) {
        return std::unexpected(parameter_failure(*failure));
    }

    return {};
}

Result<message_bus::detail::PublisherDefaults> MessageBusSettings::read_publisher_defaults() {
    // Component-level fixtures may omit file configuration. The production App
    // eagerly initializes after load; this path supplies the same live service
    // without requiring persistence or a second entry point.
    auto initialized = register_publisher_defaults();
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    auto values = params_.read_current({full_slots_name, full_bytes_name});
    if (!values) {
        return std::unexpected(parameter_failure(values.error()));
    }

    const auto slots = values->at(full_slots_name).get<std::uint32_t>();
    const auto bytes = values->at(full_bytes_name).get<std::uint64_t>();
    Result<void> valid;
    message_bus::detail::PublisherDefaults candidate = effective_;
    if (!slots) {
        valid = std::unexpected(slots.error());
    } else if (!bytes) {
        valid = std::unexpected(bytes.error());
    } else {
        candidate = {*slots, *bytes};
        valid = message_bus::detail::validate_publisher_defaults(candidate, limits_);
    }
    if (valid) {
        effective_ = candidate;
        return effective_;
    }

    // A deliberately tiny Bus can require explicit options. Do not fabricate
    // a smaller default or repair Param to a fallback that cannot fit either.
    auto fallback = message_bus::detail::validate_publisher_defaults(effective_, limits_);
    if (!fallback) {
        return std::unexpected(wrap_error(module_system::ErrorCode::BusFailed,
            module_system::ErrorOperation::AccessMessageBus, fallback.error()));
    }

    auto corrected = restore(*values);
    auto message = "Publisher defaults rejected: " + std::string(full_slots_name) + "=" +
        param::value_text(values->at(full_slots_name).value) + ", " + full_bytes_name + "=" +
        param::value_text(values->at(full_bytes_name).value) + "; retaining slots=" +
        std::to_string(effective_.slots) + ", bytes=" + std::to_string(effective_.max_message_bytes);
    if (!corrected) {
        message += "; Param correction failed (outcome may be unknown)";
        params_.report_framework(corrected.error(), std::move(message));
    } else {
        message += "; Param restored; not saved";
        params_.report_framework(valid.error(), std::move(message));
    }
    return effective_;
}
}
