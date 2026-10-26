#pragma once

// Common message interface and framework-owned delivery metadata.
// Generated classes keep their C++ facade local and encode only their described payload for transport.

#include <vinc/result.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vinc {
namespace message_bus::detail {
class MessageAccess;
}

class BaseMessage {
    friend class message_bus::detail::MessageAccess;

public:
    BaseMessage() = default;
    BaseMessage(const BaseMessage& msg) = default;
    virtual ~BaseMessage() = default;

    // Read-only delivery information supplied by the framework. These values describe
    // the received publication and are separate from the generated payload fields.
    const std::string& topic_name() const {
        return _topic_name;
    }

    // Framework publication time: unsigned Unix nanoseconds from system_clock.
    uint64_t get_message_timestamp() const {
        return _message_timestamp;
    }

    // Same-host monotonic publication time; use for ordering and elapsed time.
    uint64_t get_message_monotonic_timestamp() const {
        return _message_monotonic_timestamp;
    }

    uint64_t get_topic_count() const noexcept {
        return _topic_count;
    }

    uint64_t publisher_instance() const noexcept {
        return _publisher_instance;
    }

    const std::string& publisher_name() const {
        return _publisher_name;
    }

    virtual std::string to_string() const = 0;

    virtual uint64_t type_hash() const = 0;

    virtual std::string msg_name_str() const = 0;

    [[nodiscard]] virtual Result<void> serialize(std::vector<uint8_t>& buffer) const = 0;

    [[nodiscard]] virtual Result<void> deserialize(const std::vector<std::uint8_t>& buffer) {
        std::size_t offset = 0;
        return deserialize(buffer, offset);
    }

    // Decode into the existing binding. Failure preserves payload and offset;
    // success advances offset past one image without replacing delivery metadata.
    [[nodiscard]] virtual Result<void> deserialize(const std::vector<std::uint8_t>& buffer,
                                                 std::size_t& offset) = 0;

protected:
    BaseMessage& operator=(const BaseMessage& msg) = default;

private:
    std::string _topic_name = "unknown";
    std::string _publisher_name = "unknown";
    uint64_t _message_timestamp = 0;
    uint64_t _message_monotonic_timestamp = 0;
    uint64_t _topic_count = 0;
    uint64_t _publisher_instance = 0;
};

} //namespace vinc
