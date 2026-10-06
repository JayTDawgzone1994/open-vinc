#include <vinc/error_catalog.hpp>

namespace vinc::detail {
namespace {
template <class Entry>
std::string_view lookup(std::span<const Entry> entries, std::int32_t value) {
    for (const auto& entry : entries) {
        if (entry.value == value) {
            return entry.text;
        }
    }

    return {};
}

std::string format_record(const ErrorRecord& error, std::span<const ErrorCatalog> catalogs) {
    const ErrorCatalog* selected = nullptr;
    for (const auto& catalog : catalogs) {
        if (catalog.domain == error.domain) {
            selected = &catalog;
            break;
        }
    }

    std::string result = selected ? std::string(selected->name) : "Unknown domain";
    result += " (domain=" + std::to_string(static_cast<std::uint32_t>(error.domain)) + "): ";
    const auto description = selected ? lookup(selected->codes, error.code) : std::string_view{};
    result += description.empty() ? "Unknown error" : description;
    result += " (code=" + std::to_string(error.code) + ")";
    if (error.operation != 0) {
        const auto operation = selected ? lookup(selected->operations, error.operation) : std::string_view{};
        result += " [operation=";
        if (!operation.empty()) {
            result += operation;
            result += ":";
        }
        result += std::to_string(error.operation);
        result += "]";
    }

    if (error.native) {
        result += " [native=" + std::to_string(static_cast<std::uint32_t>(error.native.source));
        result += ":" + std::to_string(error.native.code);
        // Native provenance survives translation into an upper component's error.
        for (const auto& catalog : catalogs) {
            if (!catalog.native_text) {
                continue;
            }

            const auto text = catalog.native_text(error.native);
            if (!text.empty()) {
                result += ": " + text;
                break;
            }
        }
        result += "]";
    }

    return result;
}
}

std::string format_error(const Error& error, std::span<const ErrorCatalog> catalogs) {
    auto result = format_record(error.record(), catalogs);
    const auto causes = error.causes();
    for (std::size_t index = 0; index < causes.size(); ++index) {
        if (error.truncated() && index + 1 == causes.size()) {
            result += " <- [intermediate causes omitted]";
        }
        result += " <- ";
        result += format_record(causes[index], catalogs);
    }
    return result;
}
}
