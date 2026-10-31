#include <vinc/osal/path.hpp>
#include <vinc/osal/error.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

template <class T>
void expect_error(const vinc::Result<T>& result, vinc::osal::ErrorCode code,
                  vinc::osal::ErrorOperation operation) {
    check(!result, "invalid path accepted");
    check(result.error().is(code), "incorrect error code");
    check(result.error().is_operation(operation), "incorrect error operation");
}

void test_round_trip() {
    using namespace vinc::osal;
    const std::array<std::string_view, 5> samples{"", "relative/../not-created.txt", "space name.txt",
                                                  "\xe6\xb8\xac\xe8\xa9\xa6/\xf0\x9f\x98\x80.txt",
                                                  "trailing/"};
    for (const auto sample : samples) {
        const auto path = take(path_from_utf8(sample));
        check(take(path_to_utf8(path)) == sample, "UTF-8 round trip changed bytes");
        check(path.is_relative(), "conversion made relative path absolute");
    }

#ifdef _WIN32
    const std::array<std::string_view, 3> native_paths{"C:\\folder\\..\\file", "\\\\server\\share\\file",
                                                       "\\\\?\\C:\\folder\\file"};
#else
    const std::array<std::string_view, 2> native_paths{"/tmp/a/../b", "a\\b"};
#endif
    for (const auto sample : native_paths) {
        check(take(path_to_utf8(take(path_from_utf8(sample)))) == sample,
              "native separators or prefix changed");
    }
}

void test_invalid_encoding() {
    using namespace vinc::osal;
    // Cover continuation bytes, truncation, overlong encodings, surrogate scalars,
    // values above U+10FFFF, and legacy Big5 bytes that must never trigger guessing.
    const std::array<std::string_view, 9> invalid{
        "\x80",         "\xc2",         "\xe2\x82",         "\xc0\xaf",
        "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xa4\xa4"};
    for (const auto sample : invalid) {
        expect_error(path_from_utf8(sample), ErrorCode::InvalidEncoding, ErrorOperation::PathFromUtf8);
    }

    const std::string embedded_nul("a\0b", 3);
    expect_error(path_from_utf8(embedded_nul), ErrorCode::InvalidArgument, ErrorOperation::PathFromUtf8);
#ifdef _WIN32
    const std::filesystem::path invalid_native(std::wstring(1, static_cast<wchar_t>(0xd800)));
    const std::filesystem::path nul_native(std::wstring(L"a\0b", 3));
#else
    const std::filesystem::path invalid_native(std::string("\xff"));
    const std::filesystem::path nul_native(embedded_nul);
#endif
    expect_error(path_to_utf8(invalid_native), ErrorCode::InvalidEncoding, ErrorOperation::PathToUtf8);
    expect_error(path_to_utf8(nul_native), ErrorCode::InvalidArgument, ErrorOperation::PathToUtf8);
}

void test_file_access() {
    using namespace vinc::osal;
    const auto name = take(path_from_utf8("osal-\xe6\xb8\xac\xe8\xa9\xa6-\xf0\x9f\x98\x80.txt"));
    const auto path = std::filesystem::current_path() / name;

    // This fixture owns only this test's file; remove it on failed assertions too.
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};

    {
        std::ofstream output(path, std::ios::binary);
        output << "path-content";
        check(bool(output), "write Unicode path");
    }

    std::ifstream input(take(path_from_utf8(take(path_to_utf8(path)))), std::ios::binary);
    std::string content;
    input >> content;
    check(content == "path-content", "read Unicode path");
}
}

int main() {
    try {
        test_round_trip();
        test_invalid_encoding();
        test_file_access();
        const auto executable = take(vinc::osal::executable_path());
        check(executable.is_absolute(), "executable path must be absolute");
        check(std::filesystem::is_regular_file(executable), "executable path must exist");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
