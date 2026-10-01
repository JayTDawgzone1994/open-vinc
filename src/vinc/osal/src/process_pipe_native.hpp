#pragma once

// Private bootstrap manifest for explicit child endpoint inheritance.
// Slots are application identities; native handles are temporary launch details.

#include "pipe_native.hpp"
#include <utility>

namespace vinc::osal::detail {
inline constexpr char pipe_bootstrap_name[] = "VINC_OSAL_PIPE_BOOTSTRAP";
inline constexpr std::size_t max_pipe_bindings = 64;
// POSIX spawn targets [3, 67); duplicates are always above that range.
inline constexpr int first_pipe_source_fd = 3 + static_cast<int>(max_pipe_bindings);
void close_pipe_transfer(std::intptr_t) noexcept;
Result<std::intptr_t> duplicate_pipe_transfer(std::intptr_t);
Result<std::string> read_pipe_bootstrap();
char pipe_platform_tag() noexcept;

struct PipeTransfer {
    std::intptr_t handle = -1;
    std::uint32_t slot = 0;
    IoKind direction = IoKind::Read;
    PipeTransfer() = default;
    ~PipeTransfer() { if (handle != -1) close_pipe_transfer(handle); }
    PipeTransfer(PipeTransfer&& other) noexcept
        : handle(std::exchange(other.handle, -1)), slot(other.slot), direction(other.direction) {}
    PipeTransfer& operator=(PipeTransfer&&) = delete;
    PipeTransfer(const PipeTransfer&) = delete;
};
struct NativePipeBinding { std::uint32_t slot; IoKind direction; NativePipe* pipe; };
struct NativeInheritedPipe { std::uint32_t slot; IoKind direction; std::unique_ptr<NativePipe> pipe; };
class ChildPipe {
public:
    static Result<ChildPipe> prepare(std::span<const NativePipeBinding>);
    ChildPipe(ChildPipe&&) noexcept = default;
    ChildPipe(const ChildPipe&) = delete;
    std::span<const std::intptr_t> inheritance() const noexcept { return handles_; }
    std::string bootstrap() const;
private:
    ChildPipe() = default;
    std::vector<PipeTransfer> transfers_;
    std::vector<std::intptr_t> handles_;
};
Result<std::vector<NativeInheritedPipe>> take_native_inherited_pipes();
} // namespace vinc::osal::detail
