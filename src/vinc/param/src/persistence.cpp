#include <vinc/param/detail/persistence.hpp>
#include <vinc/osal/error.hpp>
#include <toml++/toml.hpp>
#include <fstream>
#include <sstream>

namespace vinc::param::detail {
namespace {
Error file_error(ErrorOperation operation, std::error_code native = {}) {
    return Error{ErrorCode::FileFailed, operation, osal::native_error(native)};
}

Result<Value> from_node(Type type, const toml::node* node) {
    if (!node) {
        return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
    }
    if (type == Type::String || type == Type::UInt64) {
        const auto* text = node->as_string();
        if (!text) {
            return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
        }

        return parse_value(type, text->get());
    }
    if (type == Type::Bool) {
        const auto* value = node->as_boolean();
        if (value) {
            return Value(value->get());
        }
    } else if (type == Type::Float || type == Type::Double) {
        if (const auto* value = node->as_floating_point()) {
            return parse_value(type, value_text(Value(value->get())));
        }
        if (const auto* value = node->as_integer()) {
            return parse_value(type, std::to_string(value->get()));
        }
    } else if (const auto* value = node->as_integer()) {
        return parse_value(type, std::to_string(value->get()));
    }

    return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
}

void insert_value(toml::table& table, std::string_view key, const Value& value) {
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, std::uint64_t>) {
                table.insert(key, value_text(value));
            } else if constexpr (std::is_integral_v<T> && !std::same_as<T, bool>) {
                table.insert(key, static_cast<std::int64_t>(item));
            } else if constexpr (std::is_floating_point_v<T>) {
                table.insert(key, static_cast<double>(item));
            } else {
                table.insert(key, item);
            }
        },
        value);
}

Result<SavedMap> decode_file(std::string_view text) {
    auto parsed = toml::parse(text);
    if (!parsed) {
        return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
    }

    const auto format = parsed["format"].value<std::int64_t>();
    const auto* parameters = parsed["params"].as_table();
    if (!format || *format != 1 || !parameters || parameters->size() > config::param_entry_limit ||
        parsed.table().size() != 2) {
        return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
    }
    SavedMap result;
    for (const auto& [key, node] : *parameters) {
        const auto* table = node.as_table();
        if (!valid_name(key.str()) || !table || table->size() != 3) {
            return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
        }

        const auto type_text = (*table)["type"].value<std::string>();
        if (!type_text) {
            return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
        }

        auto type = parse_type(*type_text);
        if (!type) {
            return std::unexpected(wrap_error(ErrorCode::InvalidFile, ErrorOperation::Load, type.error()));
        }

        auto defaults = from_node(*type, table->get("default"));
        auto value = from_node(*type, table->get("value"));
        if (!defaults || !value) {
            return std::unexpected(Error{ErrorCode::InvalidFile, ErrorOperation::Load});
        }

        result.emplace(std::string(key.str()), SavedEntry{std::move(*defaults), std::move(*value)});
    }
    return result;
}
}

Result<SavedMap> Persistence::load(std::filesystem::path file) {
    path_ = std::move(file);
    enabled_ = false;
    std::error_code native;
    std::filesystem::create_directories(path_.parent_path(), native);
    if (native) {
        return std::unexpected(file_error(ErrorOperation::Load, native));
    }

    const bool exists = std::filesystem::exists(path_, native);
    if (native) {
        return std::unexpected(file_error(ErrorOperation::Load, native));
    }
    if (!exists) {
        enabled_ = true;
        auto created = write({});
        if (!created) {
            return std::unexpected(created.error());
        }

        return SavedMap{};
    }
    const auto bytes = std::filesystem::file_size(path_, native);
    if (native) {
        return std::unexpected(file_error(ErrorOperation::Load, native));
    }
    if (bytes > config::param_file_bytes) {
        // A resource limit is not proof of corruption; leave the original untouched.
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Load});
    }
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        return std::unexpected(file_error(ErrorOperation::Load));
    }
    std::string content(static_cast<std::size_t>(bytes), '\0');
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
        return std::unexpected(file_error(ErrorOperation::Load));
    }

    stream.close();
    auto decoded = decode_file(content);
    if (!decoded) {
        std::filesystem::remove(path_, native);
        if (native) {
            return std::unexpected(file_error(ErrorOperation::Load, native));
        }
        enabled_ = true;
        return std::unexpected(decoded.error());
    }
    enabled_ = true;
    return decoded;
}

Result<void> Persistence::write(const SavedMap& entries) {
    if (!enabled_) {
        return std::unexpected(file_error(ErrorOperation::Save));
    }
    toml::table parameters;
    for (const auto& [name, entry] : entries) {
        toml::table table;
        table.insert("type", type_name(type_of(entry.default_value)));
        insert_value(table, "default", entry.default_value);
        insert_value(table, "value", entry.value);
        parameters.insert(name, std::move(table));
    }
    toml::table document;
    document.insert("format", 1);
    document.insert("params", std::move(parameters));
    std::ostringstream text;
    text << document;
    const auto content = text.str();
    if (content.size() > config::param_file_bytes) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Save});
    }

    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            enabled_ = false;
            return std::unexpected(file_error(ErrorOperation::Save));
        }

        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        stream.close();
        if (!stream) {
            enabled_ = false;
            return std::unexpected(file_error(ErrorOperation::Save));
        }
    }
    // Same-directory rename replaces the complete file. Never delete the destination
    // first; this provides replacement, not a machine/power-loss durability guarantee.
    std::error_code native;
    std::filesystem::rename(temporary, path_, native);
    if (native) {
        enabled_ = false;
        return std::unexpected(file_error(ErrorOperation::Save, native));
    }

    return {};
}
}
