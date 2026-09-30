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
    bool x;
    std::vector<int> y;
} testData_t;

class test : public BaseMessage
{
public:

    // --- Constants Definitions ---
    static constexpr uint32_t z = 123;


    test() = default;

    // --- Copy Semantics (深拷貝) ---
    test(const test&) = default;
    test& operator=(const test&) = default;

    // --- Move Semantics (移動權限) ---
    test(test&&) noexcept = default;
    test& operator=(test&&) noexcept = default;

    // 支援從 Data_t 直接進來
    test(testData_t&& data) noexcept : _message_data(std::move(data)) {}
    test(const testData_t& data) : _message_data(data) {}

    ~test() = default;

    static std::string to_string(const testData_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ss << "test { ";
        ss << "x: ";
        ss << data.x;
        ss << ", ";
        ss << "y: ";
        ss << "[";
        for (size_t i = 0; i < data.y.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << data.y[i];
        }
        ss << "]";
        ss << " }";
        return ss.str();
    }

    std::string to_string() const override
    {
        std::stringstream ss;
        ss << "test { ";
        ss << "x: ";
        ss << _message_data.x;
        ss << ", ";
        ss << "y: ";
        ss << "[";
        for (size_t i = 0; i < _message_data.y.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << _message_data.y[i];
        }
        ss << "]";
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
        return "test";
    }

    virtual void serialize(std::vector<uint8_t>& buffer) const override
    {
        push_raw(buffer, _message_data.x);
        {
            uint32_t sz = static_cast<uint32_t>(_message_data.y.size());
            push_raw(buffer, sz);
            if (sz > 0) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(_message_data.y.data());
                buffer.insert(buffer.end(), p, p + (sz * sizeof(int)));
            }
        }
    }

    virtual int8_t deserialize(const std::vector<uint8_t>& buffer) override
    {
        size_t offset = 0;
        
        if (!pop_raw(buffer, offset, _message_data.x)) return false;
        {
            uint32_t sz = 0;
            if (!pop_raw(buffer, offset, sz)) return false;
            _message_data.y.resize(sz);
            if (sz > 0) {
                size_t byte_count = sz * sizeof(int);
                if (offset + byte_count > buffer.size()) return false;
                std::memcpy(_message_data.y.data(), &buffer[offset], byte_count);
                offset += byte_count;
            }
        }
        
        return 0;
    }

    testData_t& data()
    {
        return _message_data;
    }
    const testData_t& data() const 
    { 
        return _message_data; 
    }

private:

    testData_t _message_data;

    static constexpr std::string_view _message_name = "test";
    static constexpr uint64_t MESSAGE_HASH = 0x24f7d27343d0501eULL;

};

}
