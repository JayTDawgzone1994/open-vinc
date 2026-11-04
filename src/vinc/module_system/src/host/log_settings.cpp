#include <vinc/module/detail/log_settings.hpp>
#include <vinc/module/error.hpp>

namespace vinc::detail {
namespace {
Error parameter_failure(const Error& cause) {
    return wrap_error(module_system::ErrorCode::ParamFailed,
                      module_system::ErrorOperation::AccessParam, cause);
}

Result<void> register_setting(param::detail::ParamManager& params, const char* name, param::Value defaults) {
    auto registered = params.register_framework(name, defaults, param::RestartHint::App);
    if (registered || !registered.error().is(param::ErrorCode::AlreadyRegistered)) {
        return registered;
    }

    // A retry may follow a committed but timed-out registration. It must not
    // replace a different declaration or accidentally treat a read failure as success.
    const auto full_name = std::string("vinc.") + name;
    auto values = params.read_current({full_name});
    if (!values) {
        return std::unexpected(values.error());
    }

    const auto& entry = values->at(full_name);
    if (entry.default_value == defaults && entry.permission == param::Permission::Limited &&
        entry.restart_hint == param::RestartHint::App) {
        return {};
    }

    return registered;
}
}

Result<void> LogSettings::register_settings() {
    auto enabled = register_setting(params_, "log_file_enabled", config::log_file_enabled);
    if (!enabled) {
        return std::unexpected(parameter_failure(enabled.error()));
    }

    auto file = register_setting(params_, "log_file_bytes", std::uint64_t{config::log_file_bytes});
    if (!file) {
        return std::unexpected(parameter_failure(file.error()));
    }

    auto total = register_setting(params_, "log_total_bytes", std::uint64_t{config::log_total_bytes});
    if (!total) {
        return std::unexpected(parameter_failure(total.error()));
    }

    registered_ = true;
    return {};
}

Result<LogSelection> LogSettings::read() {
    std::lock_guard lock(mutex_);
    if (!registered_) {
        auto registered = register_settings();
        if (!registered) {
            return std::unexpected(registered.error());
        }
    }

    return read_locked();
}

Result<std::string> LogSettings::refresh() {
    std::lock_guard lock(mutex_);
    if (!registered_) {
        return std::string{};
    }

    auto selected = read_locked();
    if (!selected) {
        return std::unexpected(selected.error());
    }

    return std::move(selected->correction);
}

Result<LogSelection> LogSettings::read_locked() {
    auto values = params_.read_current({"vinc.log_file_enabled", "vinc.log_file_bytes", "vinc.log_total_bytes"});
    if (!values) {
        return std::unexpected(parameter_failure(values.error()));
    }

    auto enabled = values->at("vinc.log_file_enabled").get<bool>();
    auto file = values->at("vinc.log_file_bytes").get<std::uint64_t>();
    auto total = values->at("vinc.log_total_bytes").get<std::uint64_t>();
    if (!enabled) {
        return std::unexpected(parameter_failure(enabled.error()));
    }

    if (!file) {
        return std::unexpected(parameter_failure(file.error()));
    }

    if (!total) {
        return std::unexpected(parameter_failure(total.error()));
    }

    accepted_.enabled = *enabled;
    if (*file > 0 && *total >= *file) {
        accepted_.max_file_bytes = *file;
        accepted_.max_total_bytes = *total;
        return LogSelection{accepted_, {}};
    }

    // Restore the last validated pair, including while files are disabled.
    // These ordinary writes neither form an atomic transaction nor implicitly save.
    // Attempt both repairs even if one fails; report the first failed operation.
    auto restored_file = params_.set_framework("log_file_bytes", accepted_.max_file_bytes);
    auto restored_total = params_.set_framework("log_total_bytes", accepted_.max_total_bytes);
    if (!restored_file) {
        return std::unexpected(parameter_failure(restored_file.error()));
    }

    if (!restored_total) {
        return std::unexpected(parameter_failure(restored_total.error()));
    }

    return LogSelection{accepted_,
        "Log capacities rejected: require file_bytes > 0 and total_bytes >= file_bytes; restored file_bytes=" +
        std::to_string(accepted_.max_file_bytes) + ", total_bytes=" +
        std::to_string(accepted_.max_total_bytes) + "; not saved. Active file settings unchanged."};
}
}
