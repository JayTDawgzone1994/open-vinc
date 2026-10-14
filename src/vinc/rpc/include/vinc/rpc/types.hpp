#pragma once
#include <vinc/rpc/error.hpp>
#include <vinc/time.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace vinc::rpc {
struct ServiceOptions {
    std::uint64_t request_bytes = config::rpc_request_bytes;
    std::uint64_t response_bytes = config::rpc_response_bytes;
    std::uint32_t slots = config::rpc_slots;
};

struct CallOptions {
    std::chrono::milliseconds timeout = config::rpc_call_timeout;
};

// Application outcome, not a framework Error. Text is bounded when returned.
struct RpcStatus {
    std::int32_t code = 0;
    std::string reason;

    static RpcStatus success() {
        return {};
    }

    static RpcStatus failure(std::int32_t code, std::string reason = {}) {
        return {code, std::move(reason)};
    }
};

template <class T> struct RpcReply {
    RpcStatus status;
    std::unique_ptr<T> response;
};

struct PumpStats {
    std::uint64_t requests = 0;
    std::uint64_t completions = 0;
};

struct Stats {
    std::uint64_t pending = 0;
    std::uint64_t request_bytes = 0;
    std::uint64_t queue_full = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t requests = 0;
    std::uint64_t completions = 0;
};
}
