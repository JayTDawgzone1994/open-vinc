// Transactional endpoint handoff: validate and pin first, spawn second, release parent ends last.
// The duplex convenience API is two independent one-way streams, not a different transport.

#include <vinc/osal/detail/operation_error.hpp>
#include <vinc/osal/detail/pipe_state.hpp>
#include <vinc/osal/detail/native.hpp>
#include <vinc/osal/detail/process_pipe_native.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <array>

namespace vinc::osal::detail {
struct ProcessAccess {
    static vinc::Result<Process> spawn(ProcessService& service, const SpawnOptions& options,
                                       const ChildPipe& child) {
        return service.spawn_impl(options, &child);
    }
};

// Pins pristine records against close/shutdown while native spawn is in flight.
struct PipeHandoff {
    std::shared_ptr<PipeState> state;
    std::vector<std::shared_ptr<PipeRecord>> records;
    bool committed = false;

    ~PipeHandoff() {
        if (!state) {
            return;
        }
        {
            std::lock_guard lock(state->mutex);
            for (const auto& record : records) {
                record->transferring = false;
                if (committed) {
                    record->closing = true;
                    record->native.reset();
                    std::erase(state->pipes, record);
                }
            }
        }
        state->monitor->wake();
    }
};
}

namespace vinc::osal {
namespace {
vinc::Result<Process> spawn_bound(ProcessService& processes, PipeService& pipes, const SpawnOptions& options,
                                  std::span<const PipeBinding> bindings,
                                  const std::array<detail::NativePipe*, 2>& outputs = {}) {
    if (bindings.size() > detail::max_pipe_bindings) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
    }
    detail::PipeHandoff handoff;
    handoff.state = detail::PipeAccess::state(pipes);
    if (!handoff.state) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::Spawn});
    }
    handoff.records.reserve(bindings.size());
    std::vector<detail::NativePipeBinding> native;
    native.reserve(bindings.size());
    auto prepared = [&]() -> vinc::Result<detail::ChildPipe> {
        std::lock_guard lock(handoff.state->mutex);
        if (!handoff.state->accepting) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::Spawn});
        }

        for (const auto& binding : bindings) {
            auto* end = std::visit(
                [](auto* endpoint) -> detail::PipeEnd* {
                    return endpoint ? &detail::PipeAccess::end(*endpoint) : nullptr;
                },
                binding.endpoint);
            if (!end || detail::PipeAccess::state(*end) != handoff.state ||
                !detail::PipeAccess::record(*end)) {
                return std::unexpected(
                    vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
            }

            auto record = detail::PipeAccess::record(*end);
            if (record->closing) {
                return std::unexpected(
                    vinc::Error{vinc::osal::ErrorCode::Closed, vinc::osal::ErrorOperation::Spawn});
            }

            if (record->used || record->transferring) {
                return std::unexpected(
                    vinc::Error{vinc::osal::ErrorCode::Busy, vinc::osal::ErrorOperation::Spawn});
            }

            if (std::any_of(native.begin(), native.end(), [&](const auto& existing_binding) {
                    return existing_binding.slot == binding.slot ||
                           existing_binding.pipe == record->native.get();
                })) {
                return std::unexpected(
                    vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
            }
            native.push_back({binding.slot, record->direction, record->native.get()});
        }

        auto result = detail::ChildPipe::prepare(native, outputs);
        if (!result) {
            return result;
        }

        for (const auto& binding : bindings) {
            auto& end = std::visit(
                [](auto* endpoint) -> detail::PipeEnd& {
                    return detail::PipeAccess::end(*endpoint);
                },
                binding.endpoint);
            auto record = detail::PipeAccess::record(end);
            record->transferring = true;
            handoff.records.push_back(std::move(record));
        }

        return result;
    }();
    if (!prepared) {
        return std::unexpected(detail::operation_error(ErrorOperation::Spawn, prepared.error()));
    }

    auto process = detail::ProcessAccess::spawn(processes, options, *prepared);
    if (process) {
        handoff.committed = true;
    }

    return process; // No allocation or fallible work after native spawn succeeds.
}
}

vinc::Result<Process> spawn_with_pipes(ProcessService& processes, PipeService& pipes,
                                       const SpawnOptions& options, std::span<const PipeBinding> bindings) {
    if (bindings.empty()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
    }

    return spawn_bound(processes, pipes, options, bindings);
}

vinc::Result<ProcessWithOutput> spawn_with_output(ProcessService& processes, PipeService& pipes,
                                                  const SpawnOptions& options,
                                                  std::span<const PipeBinding> bindings) {
    auto valid = detail::validate_options(options);
    if (!valid) {
        return std::unexpected(valid.error());
    }

    const std::array<bool, 2> capture{options.stdout_mode == StdioMode::Capture,
                                      options.stderr_mode == StdioMode::Capture};
    const auto count = static_cast<std::size_t>(capture[0]) + static_cast<std::size_t>(capture[1]);
    auto records = detail::PipeAccess::reserve(pipes, count);
    if (!records) {
        return std::unexpected(detail::operation_error(ErrorOperation::Spawn, records.error()));
    }

    struct Reservation {
        PipeService& service;
        std::span<const std::shared_ptr<detail::PipeRecord>> records;
        bool committed = false;

        ~Reservation() {
            if (!committed) {
                detail::PipeAccess::rollback(service, records);
            }
        }
    } reservation{pipes, *records};

    std::array<detail::NativePipePair, 2> pairs;
    std::array<detail::NativePipe*, 2> outputs{};
    for (std::size_t i = 0; i < capture.size(); ++i) {
        if (!capture[i]) {
            continue;
        }

        auto pair = detail::NativePipe::create_pair(true);
        if (!pair) {
            return std::unexpected(detail::operation_error(ErrorOperation::Spawn, pair.error()));
        }
        pairs[i] = std::move(*pair);
        outputs[i] = pairs[i].writer.get();
    }

    auto child = spawn_bound(processes, pipes, options, bindings, outputs);
    if (!child) {
        return std::unexpected(detail::operation_error(ErrorOperation::Spawn, child.error()));
    }
    // All allocations and capacity checks preceded spawn. Publish only moves;
    // closing every parent writer copy here lets readers eventually observe EOF.
    ProcessWithOutput result{std::move(*child), {}, {}};
    std::array<std::optional<PipeReader>*, 2> readers{&result.stdout_reader, &result.stderr_reader};
    std::size_t record = 0;
    for (std::size_t i = 0; i < capture.size(); ++i) {
        if (!capture[i]) {
            continue;
        }
        pairs[i].writer.reset();
        readers[i]->emplace(detail::PipeAccess::reader(detail::PipeAccess::commit(
            pipes, (*records)[record++], std::move(pairs[i].reader), IoKind::Read)));
    }
    reservation.committed = true;
    return result;
}

vinc::Result<std::vector<InheritedPipe>> take_inherited_pipes(PipeService& pipes) {
    auto state = detail::PipeAccess::state(pipes);
    if (!state) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::AdoptPipe});
    }
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorOperation::AdoptPipe});
        }
    }

    auto native = detail::take_native_inherited_pipes();
    if (!native) {
        return std::unexpected(native.error());
    }

    std::vector<InheritedPipe> result;
    result.reserve(native->size());
    auto records = detail::PipeAccess::reserve(pipes, native->size());
    if (!records) {
        return std::unexpected(detail::operation_error(ErrorOperation::AdoptPipe, records.error()));
    }
    // Everything below is move-only publication; it cannot allocate.
    for (std::size_t i = 0; i < native->size(); ++i) {
        auto& item = (*native)[i];
        auto end = detail::PipeAccess::commit(pipes, (*records)[i], std::move(item.pipe), item.direction);
        if (item.direction == IoKind::Read) {
            result.push_back({item.slot, detail::PipeAccess::reader(std::move(end))});
        } else {
            result.push_back({item.slot, detail::PipeAccess::writer(std::move(end))});
        }
    }

    return result;
}

vinc::Result<ProcessWithPipe> spawn_with_pipe(ProcessService& processes, PipeService& pipes,
                                              const SpawnOptions& options) {
    auto forward = pipes.create_pipe();
    if (!forward) {
        return std::unexpected(detail::operation_error(ErrorOperation::Spawn, forward.error()));
    }

    auto discard = [&] {
        detail::PipeAccess::discard(detail::PipeAccess::end(forward->reader));
        detail::PipeAccess::discard(detail::PipeAccess::end(forward->writer));
    };
    try {
        auto reverse = pipes.create_pipe();
        if (!reverse) {
            discard();
            return std::unexpected(detail::operation_error(ErrorOperation::Spawn, reverse.error()));
        }
        try {
            const std::array<PipeBinding, 2> bindings{{{0, &forward->reader}, {1, &reverse->writer}}};
            auto process = spawn_with_pipes(processes, pipes, options, bindings);
            if (process) {
                return ProcessWithPipe{std::move(*process),
                                       {std::move(reverse->reader), std::move(forward->writer)}};
            }
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->reader));
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->writer));
            discard();
            return std::unexpected(detail::operation_error(ErrorOperation::Spawn, process.error()));
        } catch (...) {
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->reader));
            detail::PipeAccess::discard(detail::PipeAccess::end(reverse->writer));
            throw;
        }
    } catch (...) {
        discard();
        throw;
    }
}

vinc::Result<DuplexPipe> take_inherited_pipe(PipeService& pipes) {
    auto inherited = take_inherited_pipes(pipes);
    if (!inherited) {
        return std::unexpected(inherited.error());
    }
    DuplexPipe result;
    for (auto& item : *inherited) {
        if (item.slot == 0 && std::holds_alternative<PipeReader>(item.endpoint)) {
            result.reader = std::get<PipeReader>(std::move(item.endpoint));
        } else if (item.slot == 1 && std::holds_alternative<PipeWriter>(item.endpoint)) {
            result.writer = std::get<PipeWriter>(std::move(item.endpoint));
        } else {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
        }
    }

    if (!result) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
    }

    return result;
}
} // namespace vinc::osal
