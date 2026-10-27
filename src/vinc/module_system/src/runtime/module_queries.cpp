#include <vinc/module/detail/module_queries.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/module_query_protocol.hpp>
#include <vinc/module/detail/error_protocol.hpp>

namespace vinc::detail {
using module_system::ErrorCode;
using module_system::ErrorOperation;

Result<void> ModuleQueries::receive(control::Frame frame) {
    std::lock_guard lock(mutex_);
    if (!frame.request || frame.request >= next_request_) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListModules});
    }

    if (frame.request != pending_) {
        return {};
    }

    if (response_) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListModules});
    }

    response_ = std::move(frame.payload);
    changed_.notify_all();
    return {};
}

Result<ModulePage> ModuleQueries::list_modules(MID after, Deadline deadline) {
    std::unique_lock lock(mutex_);
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::ListModules});
    }

    if (pending_) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::ListModules});
    }

    if (after >= central_owner || next_request_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::ListModules});
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::ListModules});
    }

    pending_ = next_request_++;
    struct Retire {
        ModuleQueries& owner;
        ~Retire() {
            owner.pending_ = 0;
            owner.response_.reset();
        }
    } retire{*this};
    control::Frame frame{control::Kind::Request, lifecycle, detail::list_modules, pending_};
    control::put_u32(frame.payload, module_query_version);
    control::put_u64(frame.payload, after);
    auto sent = session_->send(std::move(frame));
    if (!sent) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::ListModules, sent.error()));
    }

    const auto received = changed_.wait_until(lock, stop_.get_token(), deadline, [&] {
        return response_.has_value();
    });
    if (stop_.stop_requested()) {
        return std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::ListModules});
    }

    if (!received) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::ListModules});
    }

    auto body = decode_response(*response_);
    if (!body) {
        return std::unexpected(body.error());
    }

    return decode_modules(*body, after);
}
}
