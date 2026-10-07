#pragma once

#include <vinc/error.hpp>
#include <span>
#include <string_view>

namespace vinc {
// Keep code and operation entries distinct even when their domain/numbers match.
// The catalog cannot silently accept an operation table as its code descriptions.
struct ErrorCodeEntry {
    ErrorDomain domain;
    std::int32_t value;
    std::string_view text;

    template <ErrorCodeEnum Code>
    constexpr ErrorCodeEntry(Code code, std::string_view description)
        : domain(ErrorCodeTraits<Code>::domain), value(static_cast<std::int32_t>(code)), text(description) {
    }
};

struct ErrorOperationEntry {
    ErrorDomain domain;
    std::int32_t value;
    std::string_view text;

    template <ErrorOperationEnum Operation>
    constexpr ErrorOperationEntry(Operation operation, std::string_view description)
        : domain(ErrorOperationTraits<Operation>::domain), value(static_cast<std::int32_t>(operation)),
          text(description) {
    }
};

struct ErrorCatalog {
    ErrorDomain domain;
    std::string_view name;
    std::span<const ErrorCodeEntry> codes;
    std::span<const ErrorOperationEntry> operations;
    // Local adapter only, never serialized. Empty text means unsupported native source.
    std::string (*native_text)(const NativeError&) = nullptr;
};

namespace detail {
template <class Entry>
constexpr bool valid_entries_impl(ErrorDomain domain, std::span<const Entry> entries,
                                  std::int32_t minimum_value) noexcept {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (entry.domain != domain || entry.value < minimum_value || entry.text.empty()) {
            return false;
        }

        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entries[previous].value == entry.value) {
                return false;
            }
        }
    }

    return true;
}

}

constexpr bool valid_entries(ErrorDomain domain, std::span<const ErrorCodeEntry> entries) noexcept {
    return detail::valid_entries_impl(domain, entries, 0);
}

constexpr bool valid_entries(ErrorDomain domain, std::span<const ErrorOperationEntry> entries) noexcept {
    return detail::valid_entries_impl(domain, entries, 1);
}

constexpr bool valid_catalog(const ErrorCatalog& catalog) noexcept {
    // Code zero can be a legacy failure; only Result's value state means success.
    // Operation zero remains the unspecified sentinel and is not a catalog entry.
    return catalog.domain != ErrorDomain::Unknown && !catalog.name.empty() &&
           valid_entries(catalog.domain, catalog.codes) && valid_entries(catalog.domain, catalog.operations);
}

constexpr bool valid_catalogs(std::span<const ErrorCatalog> catalogs) noexcept {
    for (std::size_t index = 0; index < catalogs.size(); ++index) {
        if (!valid_catalog(catalogs[index])) {
            return false;
        }

        for (std::size_t previous = 0; previous < index; ++previous) {
            if (catalogs[index].domain == catalogs[previous].domain) {
                return false;
            }
        }
    }

    return true;
}

namespace detail {
// Composition passes immutable tables in. Common has no runtime component dependency.
std::string format_error(const Error& error, std::span<const ErrorCatalog> catalogs);
}
}
