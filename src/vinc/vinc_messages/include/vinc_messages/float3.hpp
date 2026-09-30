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
} float3Data_t;

class float3 : public BaseMessage
{
public:


    float3() = default;

    // --- Copy Semantics (深拷貝) ---
    float3(const float3&) = default;
    float3& operator=(const float3&) = default;

    // --- Move Semantics (移動權限) ---
    float3(float3&&) noexcept = default;
    float3& operator=(float3&&) noexcept = default;

    // 支援從 Data_t 直接進來
    float3(float3Data_t&& data) noexcept : _message_data(std::move(data)) {}
    float3(const float3Data_t& data) : _message_data(data) {}

    ~float3() = default;

    static std::string to_string(const float3Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "float3 { ";
        ss << "x: ";
        ss << data.x;
        ss << ", ";
        ss << "y: ";
        ss << data.y;
        ss << ", ";
        ss << "z: ";
        ss << data.z;
        ss << " }";
        return ss.str();
    }

    std::string to_string() const override
    {
        std::stringstream ss;
        ss << "float3 { ";
        ss << "x: ";
        ss << _message_data.x;
        ss << ", ";
        ss << "y: ";
        ss << _message_data.y;
        ss << ", ";
        ss << "z: ";
        ss << _message_data.z;
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
        return "float3";
    }

    virtual void serialize(std::vector<uint8_t>& buffer) const override
    {
        push_raw(buffer, _message_data.x);
        push_raw(buffer, _message_data.y);
        push_raw(buffer, _message_data.z);
    }

    virtual int8_t deserialize(const std::vector<uint8_t>& buffer) override
    {
        size_t offset = 0;
        
        if (!pop_raw(buffer, offset, _message_data.x)) return false;
        if (!pop_raw(buffer, offset, _message_data.y)) return false;
        if (!pop_raw(buffer, offset, _message_data.z)) return false;
        
        return 0;
    }

    float3Data_t& data()
    {
        return _message_data;
    }
    const float3Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    float3Data_t _message_data;

    static constexpr std::string_view _message_name = "float3";
    static constexpr uint64_t MESSAGE_HASH = 0x2049cd9a200830f2ULL;

};

}
