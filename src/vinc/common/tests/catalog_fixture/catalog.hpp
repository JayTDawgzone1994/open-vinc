#pragma once
#include <vinc/error_catalog.hpp>
#include <array>

namespace fixture {
enum class Code : std::int32_t {
    Failure = 1
};
}

namespace vinc {
template <> struct ErrorCodeTraits<fixture::Code> {
    static constexpr ErrorDomain domain = ErrorDomain::ControlChannel;
};
}

namespace fixture {
#if defined(DUPLICATE_CODE)
inline constexpr auto codes =
    std::array{vinc::ErrorCodeEntry{Code::Failure, "Fixture"}, vinc::ErrorCodeEntry{Code::Failure, "Duplicate"}};
#else
inline constexpr auto codes = std::array{vinc::ErrorCodeEntry{Code::Failure, "Fixture"}};
#endif
#if defined(WRONG_DOMAIN)
inline constexpr auto domain = vinc::ErrorDomain::Osal;
#else
inline constexpr auto domain = vinc::ErrorDomain::ControlChannel;
#endif
inline constexpr vinc::ErrorCatalog catalog{domain, "Fixture", codes, {}};
inline constexpr vinc::ErrorCatalog duplicate_catalog{domain, "Other", codes, {}};
inline constexpr vinc::ErrorCatalog updated_catalog{domain, "Updated Fixture", codes, {}};
}
