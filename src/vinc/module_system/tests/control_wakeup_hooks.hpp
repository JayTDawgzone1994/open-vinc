#pragma once

#include <vinc/control/channel.hpp>
#include <condition_variable>
namespace vinc::rpc::detail { class RpcManager; }

// Only isolated test targets compile these seams. Notifications, deadlines and
// production scheduling are unchanged; hooks never call back into the Manager.
namespace vinc::test::control_wakeup {
enum class Point { Sent, Queued, Taken };
void frame_event(Point point, control::Role role, std::uint64_t generation,
                 const control::Frame& frame);
void attached(const std::shared_ptr<control::Session>& session);
void before_wait();
void after_wait(std::cv_status status);
void pass_started(bool health_due, Deadline health_deadline);
void before_rpc_submit(rpc::detail::RpcManager&, std::uint64_t mid, const control::Frame&);
void after_rpc_submit(std::uint64_t mid, const control::Frame&, const Result<void>&);
}
