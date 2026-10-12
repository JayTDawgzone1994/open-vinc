#include <vinc/param/types.hpp>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace vinc::param {
namespace {
constexpr std::array<std::string_view, 12> names{"bool",   "int8",  "uint8",  "int16", "uint16", "int32",
                                                 "uint32", "int64", "uint64", "float", "double", "string"};

template <class T> Result<Value> number(std::string_view text) {
    T value{};
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (status != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(Error{ErrorCode::InvalidValue, ErrorOperation::Decode});
    }
    Value result(value);
    auto valid = validate_value(result);
    if (!valid) {
        return std::unexpected(valid.error());
    }

    return result;
}

bool utf8(std::string_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset++]);
        if (lead < 0x80) {
            continue;
        }
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        unsigned remaining = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            code = lead & 0x1f;
            minimum = 0x80;
            remaining = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            code = lead & 0x0f;
            minimum = 0x800;
            remaining = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            code = lead & 0x07;
            minimum = 0x10000;
            remaining = 3;
        } else {
            return false;
        }
        for (unsigned index = 0; index < remaining; ++index) {
            if (offset == text.size()) {
                return false;
            }

            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
    }
    return true;
}
}

Type type_of(const Value& value) noexcept {
    return static_cast<Type>(value.index() + 1);
}

std::string_view type_name(Type type) noexcept {
    const auto index = static_cast<std::uint32_t>(type);
    return index >= 1 && index <= names.size() ? names[index - 1] : std::string_view{};
}

Result<Type> parse_type(std::string_view text) {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (text == names[index]) {
            return static_cast<Type>(index + 1);
        }
    }
    return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Decode});
}

Result<Value> parse_value(Type type, std::string_view text) {
    switch (type) {
    case Type::Bool:
        if (text == "true" || text == "false") {
            return Value(text == "true");
        }
        break;
    case Type::Int8:
        return number<std::int8_t>(text);
    case Type::UInt8:
        return number<std::uint8_t>(text);
    case Type::Int16:
        return number<std::int16_t>(text);
    case Type::UInt16:
        return number<std::uint16_t>(text);
    case Type::Int32:
        return number<std::int32_t>(text);
    case Type::UInt32:
        return number<std::uint32_t>(text);
    case Type::Int64:
        return number<std::int64_t>(text);
    case Type::UInt64:
        return number<std::uint64_t>(text);
    case Type::Float:
        return number<float>(text);
    case Type::Double:
        return number<double>(text);
    case Type::String: {
        Value value{std::string(text)};
        auto valid = validate_value(value);
        if (!valid) {
            return std::unexpected(valid.error());
        }

        return value;
    }
    }
    return std::unexpected(Error{ErrorCode::InvalidValue, ErrorOperation::Decode});
}

std::string value_text(const Value& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, std::string>) {
                return item;
            } else if constexpr (std::same_as<T, bool>) {
                return item ? "true" : "false";
            } else {
                // max_digits10 avoids losing a floating-point value on a save/reload round trip.
                std::array<char, 128> buffer{};
                auto encoded = [&] {
                    if constexpr (std::is_floating_point_v<T>) {
                        return std::to_chars(buffer.data(), buffer.data() + buffer.size(), item,
                                             std::chars_format::general,
                                             std::numeric_limits<T>::max_digits10);
                    } else {
                        return std::to_chars(buffer.data(), buffer.data() + buffer.size(), item);
                    }
                }();
                return std::string(buffer.data(), encoded.ptr);
            }
        },
        value);
}

Result<void> validate_value(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (text->size() > config::param_string_bytes) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Set});
        }
        if (!utf8(*text)) {
            return std::unexpected(Error{ErrorCode::InvalidValue, ErrorOperation::Set});
        }
    }
    return {};
}

bool valid_segment(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char character : name) {
        const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

bool valid_name(std::string_view name) noexcept {
    const auto dot = name.find('.');
    return name.size() <= config::param_name_bytes && dot != std::string_view::npos &&
           valid_segment(name.substr(0, dot)) && valid_segment(name.substr(dot + 1));
}

Result<std::string> qualify(std::string_view module, std::string_view name) {
    if (name.size() > config::param_name_bytes || module.size() > config::param_name_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }
    std::string result;
    if (name.find('.') == std::string_view::npos) {
        result = std::string(module) + "." + std::string(name);
    } else {
        result = name;
    }
    if (!valid_name(result)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }

    return result;
}
}
