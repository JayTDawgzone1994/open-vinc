#pragma once
#include <vinc/log/error.hpp>
#include <vinc/config.hpp>
#include <fmt/format.h>
#include <array>
#include <atomic>
#include <string_view>

namespace vinc::log {
enum class Level : std::uint32_t {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3
};

namespace detail {
// Host owns this adapter through Plugin destruction. No application callback is retained.
class Access {
public:
    virtual ~Access() = default;
    virtual bool enabled(Level level) const noexcept = 0;
    virtual Result<void> write(Level level, std::string_view text) = 0;
};
struct ClientAccess;
}

class Logger {
    friend struct detail::ClientAccess;
    detail::Access* access_ = nullptr;

public:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool enabled(Level level) const noexcept {
        return access_ && access_->enabled(level);
    }

    Result<void> write(Level level, std::string_view text);

    template <class... Args>
    Result<void> format(Level level, fmt::format_string<Args...> pattern, Args&&... args) {
        if (level > Level::Debug) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Write});
        }
        if (!access_) {
            return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Write});
        }
        if (!enabled(level)) {
            return {};
        }
        // Bound framework output storage. User formatters execute now and may throw.
        std::array<char, config::log_message_bytes> buffer;
        auto result = fmt::format_to_n(buffer.data(), buffer.size(), pattern, std::forward<Args>(args)...);
        if (result.size > buffer.size()) {
            return std::unexpected(Error{ErrorCode::TooLarge, ErrorOperation::Write});
        }
        return write(level, std::string_view(buffer.data(), result.size));
    }

    template <class... Args> Result<void> error(fmt::format_string<Args...> pattern, Args&&... args) {
        return format(Level::Error, pattern, std::forward<Args>(args)...);
    }

    template <class... Args> Result<void> warn(fmt::format_string<Args...> pattern, Args&&... args) {
        return format(Level::Warn, pattern, std::forward<Args>(args)...);
    }

    template <class... Args> Result<void> info(fmt::format_string<Args...> pattern, Args&&... args) {
        return format(Level::Info, pattern, std::forward<Args>(args)...);
    }

    template <class... Args> Result<void> debug(fmt::format_string<Args...> pattern, Args&&... args) {
        return format(Level::Debug, pattern, std::forward<Args>(args)...);
    }
};

namespace detail {
struct ClientAccess {
    static void bind(Logger& logger, Access& access) noexcept {
        logger.access_ = &access;
    }
};
}
}
