#include <vinc/log/log.hpp>

namespace vinc::log {
Result<void> Logger::write(Level level, std::string_view text) {
    if (static_cast<std::uint32_t>(level) > static_cast<std::uint32_t>(Level::Debug)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Write});
    }
    if (!access_) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Write});
    }
    if (!enabled(level)) {
        return {};
    }
    if (text.size() > config::log_message_bytes) {
        return std::unexpected(Error{ErrorCode::TooLarge, ErrorOperation::Write});
    }
    return access_->write(level, text);
}
}
