#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/instance.hpp>
#include <vinc/rpc/detail/manager.hpp>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<bool> fail_next_allocation{false};
// MSVC Debug allocates tiny iterator proxies inside noexcept container constructors.
// Target a recoverable payload allocation, not that library-level termination path.
constexpr std::size_t payload_allocation_threshold = 32;
}

// Test-only allocator fault: the worker is joined before enabling this checkpoint.
void* operator new(std::size_t size) {
    if (size >= payload_allocation_threshold && fail_next_allocation.exchange(false)) {
        throw std::bad_alloc();
    }

    if (auto memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }

    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace vinc::rpc::detail {
// Drive actual admission/lease transitions at deterministic fault points.
// OS death observation is separately covered by rpc.target_exit's real process.
struct ManagerTests {
    static void check(bool value, const char* message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }

    static void run() {
        auto instance = vinc::detail::Instance::create(test::instance_name());
        check(bool(instance), "instance created");
        RpcManager manager(*instance);
        manager.worker_.request_stop();
        manager.changed_.notify_all();
        manager.worker_.join();
        auto server = std::make_shared<RpcManager::Peer>();
        server->mid = 1;
        server->name = "server";
        auto caller = std::make_shared<RpcManager::Peer>();
        caller->mid = 2;
        caller->name = "caller";
        control::Bytes registration;
        control::put_u32(registration, version);
        (void)control::put_string(registration, "echo");
        control::put_u64(registration, 11);
        control::put_u64(registration, 22);
        control::put_u64(registration, 4096);
        control::put_u64(registration, 4096);
        control::put_u32(registration, 1);
        auto registered = manager.register_service(server, frame(Register, 1, registration));
        check(bool(registered), "register real shared pool");
        auto acquire_slot = [&] {
            control::Bytes request;
            control::put_u32(request, version);
            (void)control::put_string(request, "server");
            (void)control::put_string(request, "echo");
            control::put_u64(request, 11);
            control::put_u64(request, 22);
            control::put_u64(request, 100);
            return manager.acquire(caller, frame(Acquire, 2, request));
        };
        auto first = acquire_slot();
        check(bool(first), "reserve first slot");
        auto transfer = decode_transfer(*first);
        check(bool(transfer), "descriptor valid");
        auto full = acquire_slot();
        check(!full && full.error().is(ErrorCode::QueueFull), "full never accepts a second call");
        const auto original = transfer->token;
        control::Bytes first_size;
        control::put_u64(first_size, 100);
        auto submit_failure = frame(Submit, original, first_size);
        bool allocation_failed = false;
        fail_next_allocation.store(true);
        try {
            (void)manager.transition(caller, submit_failure);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        fail_next_allocation.store(false);
        check(allocation_failed, "injected submission allocation failure");
        check(manager.services_.begin()->second.slots.front().phase == RpcManager::Phase::Granted,
              "failed notification retains reclaimable caller lease");
        caller->alive.store(false);
        manager.recover(*caller);
        check(manager.occupied_ == 0, "writer death before submit permits reuse");
        caller = std::make_shared<RpcManager::Peer>();
        caller->mid = 3;
        caller->name = "newCaller";
        auto second = acquire_slot();
        check(bool(second), "recovered capacity available");
        transfer = decode_transfer(*second);
        check(transfer && transfer->token != original, "slot reuse changes token");
        control::Bytes size;
        control::put_u64(size, 100);
        check(bool(manager.transition(caller, frame(Submit, transfer->token, size))), "submit admitted slot");
        auto reply_failure = frame(Complete, transfer->token,
                                   encode_failure(Error{ErrorCode::StorageFailed, ErrorOperation::Send}));
        allocation_failed = false;
        fail_next_allocation.store(true);
        try {
            (void)manager.transition(server, reply_failure);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        fail_next_allocation.store(false);
        check(allocation_failed, "injected completion allocation failure");
        check(manager.services_.begin()->second.slots.front().phase == RpcManager::Phase::Queued,
              "failed completion remains recoverable through callee exit");
        caller->alive.store(false);
        manager.recover(*caller);
        check(manager.occupied_ == 1, "caller death cannot steal live callee read/write lease");
        auto completed = encode_failure(Error{ErrorCode::StorageFailed, ErrorOperation::Send});
        check(bool(manager.transition(server, frame(Complete, transfer->token, completed))),
              "callee releases after copy");
        check(manager.occupied_ == 0, "abandoned reply retires safely");
        caller = std::make_shared<RpcManager::Peer>();
        caller->mid = 4;
        caller->name = "thirdCaller";
        auto third = acquire_slot();
        check(bool(third), "reserve after both-side cleanup");
        transfer = decode_transfer(*third);
        server->alive.store(false);
        manager.recover(*server);
        check(manager.occupied_ == 1, "callee death cannot steal caller still writing grant");
        check(bool(manager.transition(caller, frame(Submit, transfer->token, size))),
              "writer finishes after callee exit");
        check(manager.occupied_ == 1, "reply retained until caller acknowledgment");
        check(bool(manager.transition(caller, frame(Release, transfer->token))),
              "caller acknowledges failed transfer");
        check(manager.occupied_ == 0, "all leases retired");
        check(bool(manager.transition(caller, frame(Release, original))),
              "stale release cannot mutate future slots");
    }
};
}

int main() {
    try {
        vinc::rpc::detail::ManagerTests::run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
