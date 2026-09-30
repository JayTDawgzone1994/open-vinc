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
} int3Data_t;

class int3 : public BaseMessage
{
public:


    int3() = default;

    // --- Copy Semantics (深拷貝) ---
    int3(const int3&) = default;
    int3& operator=(const int3&) = default;

    // --- Move Semantics (移動權限) ---
    int3(int3&&) noexcept = default;
    int3& operator=(int3&&) noexcept = default;

    // 支援從 Data_t 直接進來
    int3(int3Data_t&& data) noexcept : _message_data(std::move(data)) {}
    int3(const int3Data_t& data) : _message_data(data) {}

    ~int3() = default;

    static std::string to_string(const int3Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "int3 { ";
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
        ss << "int3 { ";
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
        return "int3";
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

    int3Data_t& data()
    {
        return _message_data;
    }
    const int3Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    int3Data_t _message_data;

    static constexpr std::string_view _message_name = "int3";
    static constexpr uint64_t MESSAGE_HASH = 0x8382d7623882054aULL;

};

}
