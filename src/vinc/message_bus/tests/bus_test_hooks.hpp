#pragma once

// Compile-time fault seams for this component's private test target only.
// Production libraries contain neither these calls nor an injection registry.
#include <cstddef>
#include <functional>
#include <vinc/result.hpp>
#include <vinc/time.hpp>
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
    NotificationRoutesEncode,
    SnapshotHistory
};

Result<std::uint64_t> publication_timestamp() noexcept;
Result<void> notification_notify(osal::NotificationSender&, std::uint64_t owner);
// A per-thread pause lets snapshot tests control send/retirement interleavings.
extern thread_local std::function<void()> before_notification;
void run_notification_snapshot_tests();
bool reject_topology_admission() noexcept;
// Encoded control storage only; registry, allocator and OS buffers are separate.
void topology_storage(std::size_t history, std::size_t pages, std::size_t saved) noexcept;
void topology_worker_wake() noexcept;
void run_topology_load_tests();
void report_topology_storage();
void checkpoint(FaultPoint);
void pending_remaining(std::size_t) noexcept;
// Test-only observation before the condition variable releases the transport lock.
extern thread_local std::function<void()> before_release_wait;
// Only the waiting caller pauses, after its release ACK satisfies the predicate.
extern thread_local std::function<void(Deadline)> after_release_wait;
}
