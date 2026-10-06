// Encode, validate and consume the one-time inherited-endpoint manifest.
// Reject malformed or duplicate entries before exposing endpoints through PipeService.

#include <vinc/osal/detail/process_pipe_native.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <string_view>

namespace vinc::osal::detail {
vinc::Result<ChildPipe> ChildPipe::prepare(std::span<const NativePipeBinding> bindings,
                                           const std::array<NativePipe*, 2>& outputs) {
    ChildPipe result;
    result.transfers_.reserve(bindings.size());
    result.handles_.reserve(bindings.size());
    for (const auto& binding : bindings) {
        PipeTransfer transfer;
        auto duplicate = duplicate_pipe_transfer(binding.pipe->handle());
        if (!duplicate) {
            return std::unexpected(duplicate.error());
        }
        transfer.handle = *duplicate;
        transfer.slot = binding.slot;
        transfer.direction = binding.direction;
        result.handles_.push_back(transfer.handle);
        result.transfers_.push_back(std::move(transfer));
    }

    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (!outputs[i]) {
            continue;
        }

        auto duplicate = duplicate_pipe_transfer(outputs[i]->handle());
        if (!duplicate) {
            return std::unexpected(duplicate.error());
        }
        result.outputs_[i].handle = *duplicate;
    }

    return result;
}

std::string ChildPipe::bootstrap() const {
    std::string result = "v2:";
    result += pipe_platform_tag();
    for (std::size_t i = 0; i < transfers_.size(); ++i) {
        const auto& transfer = transfers_[i];
        const auto target = pipe_platform_tag() == 'p' ? static_cast<std::intptr_t>(3 + i) : transfer.handle;
        result += ";" + std::to_string(transfer.slot) + ":" +
                  (transfer.direction == IoKind::Read ? "r:" : "w:") + std::to_string(target);
    }

    return result;
}

namespace {
template <class T> bool number(std::string_view text, T& result) {
    if (text.empty()) {
        return false;
    }

    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

struct Parsed {
    std::uint32_t slot;
    IoKind direction;
    std::intptr_t handle;
};
}

vinc::Result<std::vector<NativeInheritedPipe>> take_native_inherited_pipes() {
    auto text = read_pipe_bootstrap();
    if (!text) {
        return std::unexpected(text.error());
    }

    const auto invalid = [] {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
    };
    if (text->size() < 5 || text->size() > max_pipe_bootstrap_bytes || text->substr(0, 3) != "v2:" ||
        (*text)[3] != pipe_platform_tag() || (*text)[4] != ';') {
        return invalid();
    }

    std::vector<Parsed> parsed;
    std::string_view remaining(*text);
    remaining.remove_prefix(5);
    while (!remaining.empty()) {
        const auto stop = remaining.find(';');
        const auto entry = remaining.substr(0, stop);
        const auto colon = entry.find(':');
        if (colon == std::string_view::npos || entry.size() <= colon + 3 || entry[colon + 2] != ':') {
            return invalid();
        }
        Parsed item{};
        if (!number(entry.substr(0, colon), item.slot) || !number(entry.substr(colon + 3), item.handle) ||
            item.handle <= 0) {
            return invalid();
        }

        if (entry[colon + 1] != 'r' && entry[colon + 1] != 'w') {
            return invalid();
        }
        item.direction = entry[colon + 1] == 'r' ? IoKind::Read : IoKind::Write;
        if (parsed.size() == max_pipe_bindings ||
            std::any_of(parsed.begin(), parsed.end(), [&](const auto& parsed_pipe) {
                return parsed_pipe.slot == item.slot || parsed_pipe.handle == item.handle;
            })) {
            return invalid();
        }

        if (pipe_platform_tag() == 'p' && item.handle != static_cast<std::intptr_t>(3 + parsed.size())) {
            return invalid();
        }
        parsed.push_back(item);
        if (stop == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(stop + 1);
        if (remaining.empty()) {
            return invalid();
        }
    }

    if (parsed.empty()) {
        return invalid();
    }
    // Allocate owning arrays before claiming any native resource. A second
    // call cannot mistake a recycled descriptor for the original launch end.
    std::vector<PipeTransfer> owned;
    owned.reserve(parsed.size());
    std::vector<NativeInheritedPipe> result;
    result.reserve(parsed.size());
    static std::atomic<bool> claimed{false};
    if (claimed.load()) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Busy, vinc::osal::ErrorOperation::AdoptPipe});
    }

    for (const auto& parsed_pipe : parsed) {
        auto valid = NativePipe::validate_handle(parsed_pipe.handle, parsed_pipe.direction);
        if (!valid) {
            return std::unexpected(valid.error());
        }
    }

    if (claimed.exchange(true)) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::Busy, vinc::osal::ErrorOperation::AdoptPipe});
    }

    for (const auto& parsed_pipe : parsed) {
        PipeTransfer transfer;
        transfer.handle = parsed_pipe.handle;
        transfer.slot = parsed_pipe.slot;
        transfer.direction = parsed_pipe.direction;
        owned.push_back(std::move(transfer));
    }

    for (auto& transfer : owned) {
        auto pipe = NativePipe::adopt_handle(transfer.handle, transfer.direction);
        if (!pipe) {
            return std::unexpected(pipe.error());
        }
        transfer.handle = -1;
        result.push_back({transfer.slot, transfer.direction, std::move(*pipe)});
    }

    return result;
}
} // namespace vinc::osal::detail
