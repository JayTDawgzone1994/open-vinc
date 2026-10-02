#include <vinc/error.hpp>

namespace vinc {

std::string error_to_string(const Error& error)
{
    // Keep a numeric fallback when the catalog grows to include named codes.
    const auto code = static_cast<std::int32_t>(error.code);

    return "Unknown VINC error (code=" + std::to_string(code) + ")";
}

} // namespace vinc
