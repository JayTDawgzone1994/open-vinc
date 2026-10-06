#include <vinc/osal/path.hpp>
#include <vinc/osal/error.hpp>
#include <vinc/osal/detail/utf8.hpp>
#include <limits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vinc::osal {
#ifdef _WIN32
namespace {
Error conversion_error(ErrorOperation operation) {
    const auto native_code = GetLastError();
    const auto code =
        native_code == ERROR_NO_UNICODE_TRANSLATION ? ErrorCode::InvalidEncoding : ErrorCode::SystemError;
    return Error{code, operation, {NativeErrorSource::Win32, native_code}};
}
}
#endif

Result<std::filesystem::path> path_from_utf8(std::string_view text) {
    constexpr auto operation = ErrorOperation::PathFromUtf8;
    if (text.find('\0') != std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, operation});
    }

    if (!detail::valid_utf8_encoding(text)) {
        return std::unexpected(Error{ErrorCode::InvalidEncoding, operation});
    }

    if (text.empty()) {
        return std::filesystem::path{};
    }

#ifdef _WIN32
    // Win32 counts UTF-8 bytes/UTF-16 code units with signed int lengths.
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, operation});
    }

    const auto input_size = static_cast<int>(text.size());
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
    if (size == 0) {
        return std::unexpected(conversion_error(operation));
    }

    std::wstring native(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, native.data(), size) ==
        0) {
        return std::unexpected(conversion_error(operation));
    }

    return std::filesystem::path(std::move(native));
#else
    // POSIX path::value_type is char: preserve validated bytes without a locale conversion.
    return std::filesystem::path(std::string(text));
#endif
}

Result<std::string> path_to_utf8(const std::filesystem::path& path) {
    constexpr auto operation = ErrorOperation::PathToUtf8;
    const auto& native = path.native();
    if (native.find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, operation});
    }

    if (native.empty()) {
        return std::string{};
    }

#ifdef _WIN32
    if (native.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, operation});
    }

    const auto input_size = static_cast<int>(native.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(), input_size, nullptr,
                                          0, nullptr, nullptr);
    if (size == 0) {
        return std::unexpected(conversion_error(operation));
    }

    std::string text(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(), input_size, text.data(), size,
                            nullptr, nullptr) == 0) {
        return std::unexpected(conversion_error(operation));
    }

    return text;
#else
    if (!detail::valid_utf8_encoding(native)) {
        return std::unexpected(Error{ErrorCode::InvalidEncoding, operation});
    }

    return native;
#endif
}
}
