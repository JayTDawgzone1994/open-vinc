#pragma once
#include <vinc/rpc/detail/protocol.hpp>
#include <deque>

namespace vinc::rpc::detail {
// Host-owned transport. Plugin owns callbacks; this interface never invokes them.
class Access {
public:
    virtual ~Access() = default;
    virtual bool living() const noexcept = 0;
    virtual Result<std::uint64_t> reserve_request() = 0;
    virtual Result<control::Bytes> exchange(control::Frame request) = 0;
    virtual Result<void> send(control::Frame request) = 0;
    virtual std::deque<control::Frame> take() = 0;
};
}
