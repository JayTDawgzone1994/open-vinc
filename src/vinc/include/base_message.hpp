#pragma once

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


    const std::string& topic_name() const
    {
        return _topic_name;
    }

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
    // 1. 偏執檢查：確保型別可以被安全地 memcpy
    static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable for pop_raw");

    // 2. 邊界檢查：剩下的 buffer 長度夠不夠讀取一個 T？
    if (offset + sizeof(T) > buffer.size()) 
    {
        return -1; // 沒肉了，讀失敗
    }

    // 3. 暴力解包：直接搬運記憶體
    std::memcpy(&value, &buffer[offset], sizeof(T));

    // 4. 指標移動：更新 offset 到下一個讀取點
    offset += sizeof(T);

    return 0;
}

inline int8_t push_string(std::vector<uint8_t>& buffer, const std::string& s) 
{
    size_t current_size = buffer.size();
    size_t string_size = s.size();
    // 4 bytes 長度頭 + 字串本體
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
