#pragma once

// Compile-time fault seams for this component's private test target only.
// Production libraries contain neither these calls nor an injection registry.
#include <cstddef>

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
    DetachSend
};

void checkpoint(FaultPoint);
void pending_remaining(std::size_t) noexcept;
}
