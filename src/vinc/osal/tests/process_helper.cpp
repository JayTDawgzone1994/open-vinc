#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <crtdbg.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
std::filesystem::path from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

#ifdef _WIN32
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}
std::string environment_value(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? "<missing>" : "";
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return utf8(value);
}
#endif

int helper(const std::vector<std::string>& args) {
    if (args.empty()) return 90;
    if (args[0] == "exit") return std::stoi(args.at(1));
    if (args[0] == "sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(args.at(1))));
        return args.size() > 2 ? std::stoi(args[2]) : 0;
    }
    if (args[0] == "abort") {
#ifdef _WIN32
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
        rlimit limit{0, 0};
        setrlimit(RLIMIT_CORE, &limit);
#endif
        std::abort();
    }
    if (args[0] == "probe-resource") {
#ifdef _WIN32
        SetEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(args.at(1)))));
#else
        const char byte = 'x';
        const auto ignored = write(std::stoi(args.at(1)), &byte, 1);
        (void)ignored;
#endif
        return 0;
    }
    if (args[0] == "null-stdio") {
        std::cout << "discarded stdout\n";
        std::cerr << "discarded stderr\n";
        return std::cin.get() == std::char_traits<char>::eof() ? 0 : 91;
    }
    if (args[0] == "report") {
        std::ofstream output(from_utf8(args.at(1)), std::ios::binary);
        if (!output) return 92;
        const auto write = [&](const std::string& value) { output << value.size() << '\n'; output.write(value.data(), static_cast<std::streamsize>(value.size())); output << '\n'; };
        write(utf8_path(std::filesystem::current_path()));
#ifdef _WIN32
        write(environment_value(L"VINC_OSAL_TEST_VALUE"));
        write(environment_value(L"VINC_OSAL_TEST_REMOVE"));
#else
        const char* value = std::getenv("VINC_OSAL_TEST_VALUE");
        write(value ? value : "<missing>");
        write(std::getenv("VINC_OSAL_TEST_REMOVE") ? "present" : "<missing>");
#endif
        for (std::size_t i = 2; i < args.size(); ++i) write(args[i]);
        return output ? 0 : 93;
    }
    return 94;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(utf8(argv[i]));
    return helper(args);
}
#else
int main(int argc, char** argv) { return helper(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
