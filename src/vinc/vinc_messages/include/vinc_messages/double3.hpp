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
    double x;
    double y;
    double z;
} double3Data_t;

class double3 : public BaseMessage
{
public:


    double3() = default;

    // --- Copy Semantics (深拷貝) ---
    double3(const double3&) = default;
    double3& operator=(const double3&) = default;

    // --- Move Semantics (移動權限) ---
    double3(double3&&) noexcept = default;
    double3& operator=(double3&&) noexcept = default;

    // 支援從 Data_t 直接進來
    double3(double3Data_t&& data) noexcept : _message_data(std::move(data)) {}
    double3(const double3Data_t& data) : _message_data(data) {}

    ~double3() = default;

    static std::string to_string(const double3Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "double3 { ";
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
        ss << "double3 { ";
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
        return "double3";
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

    double3Data_t& data()
    {
        return _message_data;
    }
    const double3Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    double3Data_t _message_data;

    static constexpr std::string_view _message_name = "double3";
    static constexpr uint64_t MESSAGE_HASH = 0x2ec6328789eda9deULL;

};

}
