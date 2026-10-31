#pragma once

#include <vinc/result.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace vinc::osal {
// Query the running executable, independently of argv[0], PATH and the working
// directory. Linux requires /proc/self/exe. The executable must still exist;
// this is a location query, not a security/identity or deployment ABI guarantee.
[[nodiscard]] Result<std::filesystem::path> executable_path();

// Strict UTF-8/native conversion only: no locale guessing, replacement characters,
// filesystem access, normalization or absolute-path resolution. Empty paths are valid.
// Embedded NUL and malformed encodings return Error. Returned values own their storage;
// allocation exceptions retain the normal C++ policy, so these functions are not noexcept.
[[nodiscard]] Result<std::filesystem::path> path_from_utf8(std::string_view text);

// Preserve native separators. A native path that cannot be represented as valid UTF-8
// returns Error; native-path OSAL APIs remain available without this conversion.
[[nodiscard]] Result<std::string> path_to_utf8(const std::filesystem::path& path);
}
