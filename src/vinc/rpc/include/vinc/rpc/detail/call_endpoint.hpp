#pragma once
#include <vinc/rpc/detail/address.hpp>
#include <vinc/rpc/types.hpp>
#include <vinc/message/storage.hpp>
#include <vinc/control/protocol.hpp>
#include <functional>

namespace vinc::rpc::detail {
struct RawResult {
    RpcStatus status;
    control::Bytes image;
};

using Completion = std::function<void(Result<RawResult>)>;

// Context integration owns this submission boundary, not the public handle.
// It must revoke admission on stop and keep entered calls safe during teardown.
// No new transport, worker or identity is created by an RpcClient.
class CallEndpoint {
public:
    virtual ~CallEndpoint() = default;

    // Own the image/completion on successful admission. Failure retains no
    // deliverable completion. Invoke completions only during pump, never inline
    // or from Host reception. Address is borrowed for this invocation only.
    virtual Result<void> submit(const Address& address, std::uint64_t request_hash,
                                std::uint64_t response_hash, message::Snapshot image,
                                CallOptions options, Completion completion) = 0;
};

struct RpcClientAccess;
}
