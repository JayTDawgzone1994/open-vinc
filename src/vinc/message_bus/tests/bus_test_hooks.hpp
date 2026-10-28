#pragma once

// Compile-time fault seams for this component's private test target only.
// Production libraries contain neither these calls nor an injection registry.
#include <cstddef>
#include <vinc/result.hpp>
#include <vinc/osal/notification.hpp>

namespace vinc::message_bus::detail::testing {
enum class FaultPoint {
    None,
    RequestSend,
    ResponseCopy,
    DescriptorDecode,
    PublisherAttach,
    CounterOpen,
    AttachmentDecode,
    DetachEncode,
    DetachSend,
    NotificationRoutesEncode
};

Result<std::uint64_t> publication_timestamp() noexcept;
Result<void> notification_notify(osal::NotificationSender&, std::uint64_t owner);
void checkpoint(FaultPoint);
void pending_remaining(std::size_t) noexcept;
}
