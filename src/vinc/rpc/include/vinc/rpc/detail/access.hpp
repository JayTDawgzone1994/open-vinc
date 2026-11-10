#pragma once
#include <vinc/rpc/detail/protocol.hpp>
#include <vinc/rpc/detail/call_endpoint.hpp>
#include <vinc/rpc/detail/call_budget.hpp>
#include <deque>

namespace vinc::rpc::detail {
// Host-owned ticket. Destruction transfers unfinished cleanup to the Host;
// waiting and payload access occur only on the calling application thread.
class BlockingCall {
public:
    virtual ~BlockingCall() = default;
    virtual Result<RawResult> wait(std::span<const std::byte> image, Deadline deadline) = 0;
};

// Host-owned transport. Plugin owns callbacks; this interface never invokes them.
class Access {
public:
    virtual ~Access() = default;
    virtual bool living() const noexcept = 0;
    virtual Result<std::uint64_t> reserve_request() = 0;
    virtual Result<control::Bytes> exchange(control::Frame request) = 0;
    virtual Result<void> send(control::Frame request) = 0;
    virtual std::deque<control::Frame> take() = 0;
    virtual Result<std::unique_ptr<BlockingCall>> start_call(
        control::Frame request, std::shared_ptr<CallReservation> reservation) = 0;
};
}
