#include <vinc/module/detail/module_locks.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <vinc/config.hpp>

namespace vinc::detail {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
Error failed(const Error& cause) {
    return wrap_error(ErrorCode::LockFailed, ErrorOperation::AccessLock, cause);
}

class HostLock final : public ContextLock {
public:
    concurrency::InterprocessMutexGuard held;
    bool publication() const noexcept override {
        return held.publication();
    }
    Result<void> unlock() noexcept override {
        auto result = held.unlock();
        if (!result) {
            return std::unexpected(failed(result.error()));
        }
        return {};
    }
    Result<void> publish_and_unlock() noexcept override {
        auto result = held.publish_and_unlock();
        if (!result) {
            return std::unexpected(failed(result.error()));
        }
        return {};
    }
};
}

ModuleLocks::ModuleLocks(std::shared_ptr<control::Session> session, MID mid)
    : session_(std::move(session)), mid_(mid) {
}

void ModuleLocks::stop() noexcept {
    // Registered stop callbacks only wake internal waits; never call application code.
    stop_.request_stop();
}

Result<void> ModuleLocks::receive(control::Frame frame) {
    std::lock_guard lock(mutex_);
    if (!frame.request || frame.request >= next_request_) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::AccessLock});
    }
    auto found = pending_.find(frame.request);
    if (found == pending_.end()) {
        return {};
    }
    if (found->second->response) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::AccessLock});
    }
    found->second->response = std::move(frame.payload);
    found->second->changed.notify_all();
    return {};
}

Result<std::shared_ptr<concurrency::InterprocessMutex>> ModuleLocks::lookup(std::uint64_t id, Deadline deadline) {
    std::unique_lock lock(mutex_);
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::AccessLock});
    }
    if (auto found = locks_.find(id); found != locks_.end()) {
        return found->second;
    }
    if (!id || id > config::module_lock_limit) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::AccessLock});
    }
    if (pending_.size() >= config::module_lock_pending_limit || next_request_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::AccessLock});
    }
    auto pending = std::make_shared<Pending>();
    const auto request = next_request_++;
    pending_.emplace(request, pending);
    // The unique_lock outlives this cleanup; every failure/exception retires the
    // slot, including exceptions after response receipt or native mapping.
    struct ErasePending {
        ModuleLocks& owner;
        std::unique_lock<std::mutex>& lock;
        std::uint64_t request;
        ~ErasePending() {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            owner.pending_.erase(request);
        }
    } cleanup{*this, lock, request};

    control::Frame frame{control::Kind::Request, lifecycle, attach_lock, request};
    control::put_u32(frame.payload, lock_protocol_version);
    control::put_u64(frame.payload, id);
    auto sent = session_->send(std::move(frame));
    if (!sent) {
        return std::unexpected(failed(sent.error()));
    }
    const bool received = pending->changed.wait_until(lock, stop_.get_token(), deadline, [&] {
        return pending->response.has_value();
    });
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::AccessLock});
    }
    if (!received) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::AccessLock});
    }
    auto payload = decode_response(*pending->response);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    control::Reader reader(*payload);
    const auto version = reader.u32();
    const auto resource = reader.u64();
    const auto mid = reader.u64();
    auto name = reader.string();
    if (!reader.done() || version != lock_protocol_version || resource != id || mid != mid_) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::AccessLock});
    }
    lock.unlock();
    // Potentially blocking native mapping never holds the receive/cache mutex.
    auto opened = concurrency::InterprocessMutex::open(name, mid_);
    if (!opened) {
        return std::unexpected(failed(opened.error()));
    }
    auto mutex = std::make_shared<concurrency::InterprocessMutex>(std::move(*opened));
    lock.lock();
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::AccessLock});
    }
    return locks_.try_emplace(id, std::move(mutex)).first->second;
}

Result<std::unique_ptr<ContextLock>> ModuleLocks::lock(std::uint64_t id, Deadline deadline) {
    auto mutex = lookup(id, deadline);
    if (!mutex) {
        return std::unexpected(mutex.error());
    }
    // Allocate the Host-owned wrapper before acquiring shared ownership.
    auto lease = std::make_unique<HostLock>();
    auto acquired = (*mutex)->lock_until(deadline, stop_.get_token());
    if (!acquired) {
        return std::unexpected(failed(acquired.error()));
    }
    lease->held = std::move(*acquired);
    return std::unique_ptr<ContextLock>(std::move(lease));
}
}
