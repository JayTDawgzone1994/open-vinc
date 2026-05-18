#pragma once

#include <iostream>
#include <cstdint>
#include <string>
#include <sstream>
#include <string_view>

#include <base_message.hpp>

${DO_INCLUDE_DEPENDENT_MESSAGES}

namespace holon
{

typedef struct
{
    ${DO_ADD_MESSAGE_FIELDS}
} ${MESSAGE_NAME}Data_t;

class ${MESSAGE_NAME} : public BaseMessage
{
public:

    ${DO_ADD_CONSTANTS_DEFINITIONS}

    ${MESSAGE_NAME}() = default;

    // --- Copy Semantics (深拷貝) ---
    ${MESSAGE_NAME}(const ${MESSAGE_NAME}&) = default;
    ${MESSAGE_NAME}& operator=(const ${MESSAGE_NAME}&) = default;

    // --- Move Semantics (移動權限) ---
    ${MESSAGE_NAME}(${MESSAGE_NAME}&&) noexcept = default;
    ${MESSAGE_NAME}& operator=(${MESSAGE_NAME}&&) noexcept = default;

    // 支援從 Data_t 直接進來
    ${MESSAGE_NAME}(${MESSAGE_NAME}Data_t&& data) noexcept : _message_data(std::move(data)) {}
    ${MESSAGE_NAME}(const ${MESSAGE_NAME}Data_t& data) : _message_data(data) {}

    ~${MESSAGE_NAME}() = default;

    static std::string to_string(const ${MESSAGE_NAME}Data_t& data)
    {
        std::stringstream ss;
        ss << std::boolalpha;
        ${DO_DATA_STRUCT_TO_STRING}
        return ss.str();
    }

    std::string to_string() const override
    {
        std::stringstream ss;
        ${DO_MESSAGE_TO_STRING_FIELDS}
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
        return "${MESSAGE_NAME}";
    }

    virtual void serialize(std::vector<uint8_t>& buffer) const override
    {
        ${DO_MESSAGE_SERIALIZE_FIELDS}
    }

    virtual int8_t deserialize(const std::vector<uint8_t>& buffer) override
    {
        size_t offset = 0;
        
        ${DO_MESSAGE_DESERIALIZE_FIELDS}
        
        return 0;
    }

    ${MESSAGE_NAME}Data_t& data()
    {
        return _message_data;
    }
    const ${MESSAGE_NAME}Data_t& data() const 
    { 
        return _message_data; 
    }

private:

    ${MESSAGE_NAME}Data_t _message_data;

    static constexpr std::string_view _message_name = "${MESSAGE_NAME}";
    static constexpr uint64_t MESSAGE_HASH = ${MESSAGE_TYPE_HASH};

};

}
