#pragma once
#include <vinc/log/log.hpp>
#include <vinc/control/channel.hpp>
#include <atomic>
#include <mutex>

namespace vinc::log::detail {
inline constexpr std::uint16_t domain = static_cast<std::uint16_t>(control::Domain::Log);
inline constexpr std::uint16_t record_operation = 1;
inline constexpr std::uint16_t level_operation = 2;
inline constexpr std::uint16_t fence_operation = 3;
inline constexpr std::uint16_t statistics_operation = 4;
inline constexpr std::uint32_t protocol_version = 1;

struct Message {
    Level level;
    std::int64_t timestamp_us;
    std::uint64_t sequence;
    std::uint64_t rejected;
    std::string text;
};

Result<control::Frame> encode_message(const Message& message);
Result<Message> decode_message(const control::Frame& frame);

// The Host owns this endpoint; the Plugin sees only Access's virtual interface.
class Host final : public Access {
    std::shared_ptr<control::Session> session_;
    std::optional<Error> connection_failure_;
    std::atomic<Level> level_{Level::Info};
    std::mutex mutex_;
    std::uint64_t sequence_ = 0;
    std::uint64_t rejected_ = 0;
    Deadline next_statistics_{};

public:
    static std::unique_ptr<Host> connect(control::Service& service, std::string endpoint,
                                         control::Options options);
    ~Host();
    bool enabled(Level level) const noexcept override;
    Result<void> write(Level level, std::string_view text) override;
    void poll();
};
}
