#include "pipe_state.hpp"
#include "process_pipe_native.hpp"
#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <array>

namespace vinc::osal::detail {
struct ProcessAccess {
    static Result<Process> spawn(ProcessService& service, const SpawnOptions& options, const ChildPipe& child) {
        return service.spawn_impl(options, &child);
    }
};
// Pins pristine records against close/shutdown while native spawn is in flight.
struct PipeHandoff {
    std::shared_ptr<PipeState> state;
    std::vector<std::shared_ptr<PipeRecord>> records;
    bool committed = false;
    ~PipeHandoff() {
        if (!state) return;
        { std::lock_guard lock(state->mutex);
          for (const auto& record : records) {
              record->transferring = false;
              if (committed) {
                  record->closing = true;
                  record->native.reset();
                  std::erase(state->pipes, record);
              }
          } }
        state->monitor->wake();
    }
};
}
namespace vinc::osal {
Result<Process> spawn_with_pipes(ProcessService& processes, PipeService& pipes,
    const SpawnOptions& options, std::span<const PipeBinding> bindings) {
    if (bindings.empty() || bindings.size() > detail::max_pipe_bindings)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
    detail::PipeHandoff handoff;
    handoff.state = detail::PipeAccess::state(pipes);
    if (!handoff.state) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Spawn});
    handoff.records.reserve(bindings.size());
    std::vector<detail::NativePipeBinding> native;
    native.reserve(bindings.size());
    auto prepared = [&]() -> Result<detail::ChildPipe> {
        std::lock_guard lock(handoff.state->mutex);
        if (!handoff.state->accepting) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Spawn});
        for (const auto& binding : bindings) {
            auto* end = std::visit([](auto* p) -> detail::PipeEnd* { return p ? &detail::PipeAccess::end(*p) : nullptr; }, binding.endpoint);
            if (!end || detail::PipeAccess::state(*end) != handoff.state || !detail::PipeAccess::record(*end))
                return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
            auto record = detail::PipeAccess::record(*end);
            if (record->closing) return std::unexpected(OsError{ErrorCode::Closed, Operation::Spawn});
            if (record->used || record->transferring)
                return std::unexpected(OsError{ErrorCode::Busy, Operation::Spawn});
            if (std::any_of(native.begin(), native.end(), [&](const auto& b) {
                return b.slot == binding.slot || b.pipe == record->native.get();
            })) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
            native.push_back({binding.slot, record->direction, record->native.get()});
        }
        auto result = detail::ChildPipe::prepare(native);
        if (!result) return result;
        for (const auto& binding : bindings) {
            auto& end = std::visit([](auto* p) -> detail::PipeEnd& { return detail::PipeAccess::end(*p); }, binding.endpoint);
            auto record = detail::PipeAccess::record(end);
            record->transferring = true;
            handoff.records.push_back(std::move(record));
        }
        return result;
    }();
    if (!prepared) return std::unexpected(prepared.error());
    auto process = detail::ProcessAccess::spawn(processes, options, *prepared);
    if (process) handoff.committed = true;
    return process; // No allocation or fallible work after native spawn succeeds.
}

Result<std::vector<InheritedPipe>> take_inherited_pipes(PipeService& pipes) {
    auto state = detail::PipeAccess::state(pipes);
    if (!state) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::AdoptPipe});
    { std::lock_guard lock(state->mutex);
      if (!state->accepting) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::AdoptPipe}); }
    auto native = detail::take_native_inherited_pipes();
    if (!native) return std::unexpected(native.error());
    std::vector<InheritedPipe> result;
    result.reserve(native->size());
    auto records = detail::PipeAccess::reserve(pipes, native->size());
    if (!records) return std::unexpected(records.error());
    // Everything below is move-only publication; it cannot allocate.
    for (std::size_t i = 0; i < native->size(); ++i) {
        auto& item = (*native)[i];
        auto end = detail::PipeAccess::commit(pipes, (*records)[i], std::move(item.pipe), item.direction);
        if (item.direction == IoKind::Read)
            result.push_back({item.slot, detail::PipeAccess::reader(std::move(end))});
        else result.push_back({item.slot, detail::PipeAccess::writer(std::move(end))});
    }
    return result;
}

Result<ProcessWithPipe> spawn_with_pipe(ProcessService& processes, PipeService& pipes, const SpawnOptions& options) {
    auto forward = pipes.create_pipe();
    if (!forward) return std::unexpected(forward.error());
    auto discard = [&] {
        detail::PipeAccess::discard(detail::PipeAccess::end(forward->reader));
        detail::PipeAccess::discard(detail::PipeAccess::end(forward->writer));
    };
    try {
        auto reverse = pipes.create_pipe();
        if (!reverse) { discard(); return std::unexpected(reverse.error()); }
        try {
            const std::array<PipeBinding, 2> bindings{{{0, &forward->reader}, {1, &reverse->writer}}};
            auto process = spawn_with_pipes(processes, pipes, options, bindings);
            if (process) return ProcessWithPipe{std::move(*process), {std::move(reverse->reader), std::move(forward->writer)}};
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->reader));
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->writer));
            discard();
            return std::unexpected(process.error());
        } catch (...) {
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->reader));
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->writer));
            throw;
        }
    } catch (...) { discard(); throw; }
}
Result<DuplexPipe> take_inherited_pipe(PipeService& pipes) {
    auto inherited = take_inherited_pipes(pipes);
    if (!inherited) return std::unexpected(inherited.error());
    DuplexPipe result;
    for (auto& item : *inherited) {
        if (item.slot == 0 && std::holds_alternative<PipeReader>(item.endpoint))
            result.reader = std::get<PipeReader>(std::move(item.endpoint));
        else if (item.slot == 1 && std::holds_alternative<PipeWriter>(item.endpoint))
            result.writer = std::get<PipeWriter>(std::move(item.endpoint));
        else return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
    }
    if (!result) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
    return result;
}
} // namespace vinc::osal
