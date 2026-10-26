#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vinc {
// Owned diagnostic values, not a shared-memory ABI. Text is bounded and terminated.
struct ModuleContractReport {
    static constexpr std::size_t reason_capacity = 256;
    static constexpr std::size_t file_capacity = 768;
    static constexpr std::size_t function_capacity = 256;
    std::array<char, reason_capacity> reason{};
    std::array<char, file_capacity> file{};
    std::array<char, function_capacity> function{};
    std::uint32_t line = 0;
    bool has_index = false;
    bool text_truncated = false;
    std::uint64_t index = 0;
    std::uint64_t size = 0;
};
}
