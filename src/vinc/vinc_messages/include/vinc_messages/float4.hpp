#pragma once

#include <iostream>
#include <cstdint>
#include <string>
#include <sstream>
#include <string_view>

#include <base_message.hpp>


namespace vinc
{

typedef struct
{
    float x;
    float y;
    float z;
    float w;
} float4Data_t;

class float4 : public BaseMessage
{
public:


    float4() = default;

    // --- Copy Semantics (深拷貝) ---
    float4(const float4&) = default;
    float4& operator=(const float4&) = default;

    // --- Move Semantics (移動權限) ---
    float4(float4&&) noexcept = default;
    float4& operator=(float4&&) noexcept = default;

    // 支援從 Data_t 直接進來
    float4(float4Data_t&& data) noexcept : _message_data(std::move(data)) {}
    float4(const float4Data_t& data) : _message_data(data) {}

    ~float4() = default;

    static std::string to_string(const float4Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "float4 { ";
        ss << "x: ";
        ss << data.x;
        ss << ", ";
        ss << "y: ";
        ss << data.y;
        ss << ", ";
        ss << "z: ";
        ss << data.z;
        ss << ", ";
        ss << "w: ";
        ss << data.w;
        ss << " }";
        return ss.str();
    }

    std::string to_string() const override
    {
        std::stringstream ss;
        ss << "float4 { ";
        ss << "x: ";
        ss << _message_data.x;
        ss << ", ";
        ss << "y: ";
        ss << _message_data.y;
        ss << ", ";
        ss << "z: ";
        ss << _message_data.z;
        ss << ", ";
        ss << "w: ";
        ss << _message_data.w;
        ss << " }";
        return ss.str();
    }

    static constexpr uint64_t get_type_hash_static() 
    { 
        return MESSAGE_HASH; 
    }

    uint64_t type_hash() const override
    {
        return MESSAGE_HASH;
    }

    std::string msg_name_str() const override
    {
        return "float4";
    }

    virtual void serialize(std::vector<uint8_t>& buffer) const override
    {
        push_raw(buffer, _message_data.x);
        push_raw(buffer, _message_data.y);
        push_raw(buffer, _message_data.z);
        push_raw(buffer, _message_data.w);
    }

    virtual int8_t deserialize(const std::vector<uint8_t>& buffer) override
    {
        size_t offset = 0;
        
        if (!pop_raw(buffer, offset, _message_data.x)) return false;
        if (!pop_raw(buffer, offset, _message_data.y)) return false;
        if (!pop_raw(buffer, offset, _message_data.z)) return false;
        if (!pop_raw(buffer, offset, _message_data.w)) return false;
        
        return 0;
    }

    float4Data_t& data()
    {
        return _message_data;
    }
    const float4Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    float4Data_t _message_data;

    static constexpr std::string_view _message_name = "float4";
    static constexpr uint64_t MESSAGE_HASH = 0xa83f88d5767f0339ULL;

};

}
