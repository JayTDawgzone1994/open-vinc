#pragma once

// Common message interface and framework-owned delivery metadata.
// Generated classes keep their C++ facade local and encode only their described payload for transport.


#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <typeindex>
#include <limits>
#include <type_traits>


namespace vinc
{
namespace bus::detail { class MessageAccess; }

class BaseMessage
{
friend class AxonBus;
friend class bus::detail::MessageAccess;
public:

    BaseMessage() = default;
    BaseMessage(const BaseMessage& msg) = default;
    virtual ~BaseMessage() = default;


    // Read-only delivery information supplied by the framework. These values describe
    // the received publication and are separate from the generated payload fields.
    const std::string& topic_name() const
    {
        return _topic_name;
    }

    // Same-host monotonic publication time in nanoseconds, not a UTC wall-clock date.
    uint64_t get_message_timestamp() const
    {
        return _message_timestamp;
    }
    uint64_t get_topic_count() const noexcept { return _topic_count; }
    uint64_t publisher_instance() const noexcept { return _publisher_instance; }

    const std::string& publisher_name() const
    {
        return _publisher_name;
    }

    virtual std::string to_string() const = 0;

    virtual uint64_t type_hash() const = 0;

    virtual std::string msg_name_str() const = 0;

    virtual void serialize(std::vector<uint8_t>& buffer) const = 0;
    virtual int8_t deserialize(const std::vector<uint8_t>& buffer) 
    {
        size_t offset = 0;
        return deserialize(buffer, offset);
    }
    virtual int8_t deserialize(const std::vector<uint8_t>& buffer, size_t& offset) = 0;

protected:
    BaseMessage& operator =(const BaseMessage& msg) = default;

private:
    std::string _topic_name = "unknown";
    std::string _publisher_name = "unknown";
    uint64_t _message_timestamp = 0;
    uint64_t _topic_count = 0;
    uint64_t _publisher_instance = 0;
};

// Legacy native-layout helpers retained for older message code. They do not provide
// the relocatable, endian-defined format used by the new generated message storage.
template<typename T>
inline int8_t push_raw(std::vector<uint8_t>& buffer, const T& value) 
{
    static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable for push_raw");
    
    size_t current_size = buffer.size();
    size_t data_size = sizeof(T);

    if (current_size + data_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) 
    {
        return -1;
    }

    buffer.resize(current_size + data_size);
    std::memcpy(buffer.data() + current_size, &value, data_size);
    
    return 0;
}

template<typename T>
inline int8_t pop_raw(const std::vector<uint8_t>& buffer, size_t& offset, T& value) 
{
    // Raw copying is valid only for trivially copyable local representations.
    static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable for pop_raw");

    // Check that one complete value remains before reading it.
    if (offset + sizeof(T) > buffer.size()) 
    {
        return -1; // Not enough bytes for this value.
    }

    // Copy the native representation; this helper performs no endian conversion.
    std::memcpy(&value, &buffer[offset], sizeof(T));

    // Advance only after the value has been copied successfully.
    offset += sizeof(T);

    return 0;
}

inline int8_t push_string(std::vector<uint8_t>& buffer, const std::string& s) 
{
    size_t current_size = buffer.size();
    size_t string_size = s.size();
    // A native uint32 length prefix followed by the string bytes.
    size_t total_needed = current_size + sizeof(uint32_t) + string_size;

    if (total_needed > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) 
    {
        return -1;
    }

    uint32_t len = static_cast<uint32_t>(s.size());
    push_raw(buffer, len);

    if (len > 0) 
    {
        size_t offset = buffer.size();
        buffer.resize(offset + len);
        std::memcpy(buffer.data() + offset, s.data(), len);
    }
    return 0;
}


} //namespace vinc
