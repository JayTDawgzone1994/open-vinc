#pragma once
#include <vinc/osal/process.hpp>
#include <vinc/osal/pipe.hpp>
#include <variant>

namespace vinc::osal {
// Caller-defined identities, not native descriptors. At most 64 per spawn.
struct PipeBinding {
    std::uint32_t slot;
    std::variant<PipeReader*, PipeWriter*> endpoint;
};
struct InheritedPipe {
    std::uint32_t slot;
    std::variant<PipeReader, PipeWriter> endpoint;
};
// Explicit ownership handoff. Endpoints must belong to pipes and have never
// accepted I/O. Success closes the parent's handed-off ends; failure preserves
// them. No concurrent move/destruction of binding objects or referenced ends.
// These async endpoints do not redirect arbitrary child stdio.
[[nodiscard]] Result<Process> spawn_with_pipes(ProcessService&, PipeService&,
    const SpawnOptions&, std::span<const PipeBinding>);
// Consume the private manifest once during child initialization. Failure after
// claiming native resources closes them; this is not a retryable adopt API.
[[nodiscard]] Result<std::vector<InheritedPipe>> take_inherited_pipes(PipeService&);

// Two independent one-way pipes, slots 0 (child reader) and 1 (child writer).
struct DuplexPipe {
    PipeReader reader;
    PipeWriter writer;
    explicit operator bool() const noexcept { return reader && writer; }
    void close() noexcept { reader.close(); writer.close(); }
};
struct ProcessWithPipe { Process process; DuplexPipe pipe; };
[[nodiscard]] Result<ProcessWithPipe> spawn_with_pipe(ProcessService&, PipeService&, const SpawnOptions&);
[[nodiscard]] Result<DuplexPipe> take_inherited_pipe(PipeService&);
} // namespace vinc::osal
