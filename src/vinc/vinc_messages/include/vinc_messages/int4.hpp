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
    int x;
    int y;
    int z;
    int w;
} int4Data_t;

class int4 : public BaseMessage
{
public:


    int4() = default;

    // --- Copy Semantics (深拷貝) ---
    int4(const int4&) = default;
    int4& operator=(const int4&) = default;

    // --- Move Semantics (移動權限) ---
    int4(int4&&) noexcept = default;
    int4& operator=(int4&&) noexcept = default;

    // 支援從 Data_t 直接進來
    int4(int4Data_t&& data) noexcept : _message_data(std::move(data)) {}
    int4(const int4Data_t& data) : _message_data(data) {}

    ~int4() = default;

    static std::string to_string(const int4Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "int4 { ";
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
        ss << "int4 { ";
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
        return "int4";
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

    int4Data_t& data()
    {
        return _message_data;
    }
    const int4Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    int4Data_t _message_data;

    static constexpr std::string_view _message_name = "int4";
    static constexpr uint64_t MESSAGE_HASH = 0xcdf8ef3e2469ee5eULL;

};

}
