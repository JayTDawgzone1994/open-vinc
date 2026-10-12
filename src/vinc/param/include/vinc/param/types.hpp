#pragma once
#include <vinc/param/error.hpp>
#include <vinc/config.hpp>
#include <map>
#include <concepts>
#include <string>
#include <variant>
#include <string_view>

namespace vinc::param {
enum class Type : std::uint32_t {
    Bool = 1,
    Int8 = 2,
    UInt8 = 3,
    Int16 = 4,
    UInt16 = 5,
    Int32 = 6,
    UInt32 = 7,
    Int64 = 8,
    UInt64 = 9,
    Float = 10,
    Double = 11,
    String = 12
};
enum class Permission : std::uint32_t {
    FrameworkOnly = 0,
    Open = 1
};
using Value = std::variant<bool, std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
                           std::uint32_t, std::int64_t, std::uint64_t, float, double, std::string>;
template <class T>
concept ParamType =
    std::same_as<T, bool> || std::same_as<T, std::int8_t> || std::same_as<T, std::uint8_t> ||
    std::same_as<T, std::int16_t> || std::same_as<T, std::uint16_t> || std::same_as<T, std::int32_t> ||
    std::same_as<T, std::uint32_t> || std::same_as<T, std::int64_t> || std::same_as<T, std::uint64_t> ||
    std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::string>;

Type type_of(const Value& value) noexcept;
std::string_view type_name(Type type) noexcept;
Result<Type> parse_type(std::string_view text);
Result<Value> parse_value(Type type, std::string_view text);
std::string value_text(const Value& value);
Result<void> validate_value(const Value& value);
bool valid_segment(std::string_view name) noexcept;
bool valid_name(std::string_view name) noexcept;
Result<std::string> qualify(std::string_view module, std::string_view name);

struct Entry {
    Value value;
    Value default_value;
    Permission permission = Permission::FrameworkOnly;
    std::uint64_t version = 0;
    std::uint64_t timestamp = 0; // Same-host steady-clock nanoseconds.

    template <ParamType T> Result<T> get() const {
        const auto* found = std::get_if<T>(&value);
        if (!found) {
            return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Update});
        }

        return *found;
    }

    friend bool operator==(const Entry&, const Entry&) = default;
};

using ParamMap = std::map<std::string, Entry, std::less<>>;

struct Snapshot {
    std::uint64_t revision = 0;
    ParamMap entries;
};
}
