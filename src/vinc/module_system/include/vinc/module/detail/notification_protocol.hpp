#pragma once

#include <vinc/module/module_info.hpp>
#include <vinc/module/detail/error_protocol.hpp>

namespace vinc::detail {
inline constexpr std::uint32_t notification_report_version = 1;

inline void encode_notification_health(control::Bytes& bytes, const NotificationHealth& health) {
    control::put_u32(bytes, health.degraded ? 1 : 0);
    control::put_u64(bytes, health.target_mid);
    control::put_u64(bytes, health.failures);
    control::put_u64(bytes, health.reopen_attempts);
    control::put_u64(bytes, health.recoveries);
    control::put_u64(bytes, health.first_failure);
    auto error = health.last_error ? encode_error(*health.last_error) : control::Bytes{};
    control::put_u32(bytes, static_cast<std::uint32_t>(error.size()));
    bytes.insert(bytes.end(), error.begin(), error.end());
}

inline Result<NotificationHealth> decode_notification_health(control::Reader& reader) {
    NotificationHealth health;
    const auto degraded = reader.u32();
    health.target_mid = reader.u64();
    health.failures = reader.u64();
    health.reopen_attempts = reader.u64();
    health.recoveries = reader.u64();
    health.first_failure = reader.u64();
    const auto error = reader.string();
    if (degraded > 1 || error.size() > error_wire_header_bytes + error_wire_max_records * error_wire_record_bytes ||
        (degraded && (health.failures == 0 || error.empty()))) {
        return std::unexpected(module_protocol_error());
    }
    health.degraded = degraded != 0;
    if (!error.empty()) {
        auto decoded = decode_error(std::as_bytes(std::span(error.data(), error.size())));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        health.last_error = *decoded;
    }
    return health;
}

inline void encode_notifications(control::Bytes& bytes, const ModuleNotifications& notifications) {
    control::put_u32(bytes, notification_report_version);
    control::put_u32(bytes, notifications.observed ? 1 : 0);
    encode_notification_health(bytes, notifications.receiver);
    encode_notification_health(bytes, notifications.local_sender);
    encode_notification_health(bytes, notifications.publisher_routes);
}

inline Result<ModuleNotifications> decode_notifications(control::Reader& reader) {
    const auto version = reader.u32();
    const auto observed = reader.u32();
    if (version != notification_report_version || observed > 1) {
        return std::unexpected(module_protocol_error());
    }
    auto receiver = decode_notification_health(reader);
    auto sender = decode_notification_health(reader);
    auto routes = decode_notification_health(reader);
    if (!receiver || !sender || !routes) {
        return std::unexpected(module_protocol_error());
    }
    return ModuleNotifications{observed != 0, *receiver, *sender, *routes};
}
}
