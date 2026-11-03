#include <cstdio>
#include <expected>
#include <string>

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error Vinc requires C++23 std::expected
#endif

int main() {
    std::printf("pointer=%zu;string=%zu;", sizeof(void*), sizeof(std::string));
#ifdef _MSC_VER
    std::printf("msvc=%d;iterator=%d;", _MSC_VER, _ITERATOR_DEBUG_LEVEL);
#ifdef _DLL
    std::printf("crt=dll;");
#else
    std::printf("crt=static;");
#endif
#ifdef _DEBUG
    std::printf("crt_debug=1;");
#else
    std::printf("crt_debug=0;");
#endif
#endif
#ifdef _MSVC_STL_VERSION
    std::printf("msvc_stl=%d;update=%ld;", _MSVC_STL_VERSION, _MSVC_STL_UPDATE);
#endif
#ifdef __GLIBCXX__
    std::printf("libstdcxx=%d;cxx11=%d;", __GLIBCXX__, _GLIBCXX_USE_CXX11_ABI);
#ifdef _GLIBCXX_DEBUG
    std::printf("glibcxx_debug=1;");
#endif
#endif
#ifdef _LIBCPP_VERSION
    std::printf("libcxx=%d;abi=%d;", _LIBCPP_VERSION, _LIBCPP_ABI_VERSION);
#ifdef _LIBCPP_ABI_UNSTABLE
    std::printf("libcxx_unstable=1;");
#endif
#endif
#if defined(_M_X64) || defined(__x86_64__)
    std::printf("arch=x86_64;");
#elif defined(_M_ARM64) || defined(__aarch64__)
    std::printf("arch=arm64;");
#else
#error Unsupported SDK architecture
#endif
    return 0;
}
