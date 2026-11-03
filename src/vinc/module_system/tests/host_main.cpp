// Test-only process entry for isolated Manager/Runtime tests. Production Apps
// dispatch through apps/vinc/src/main.cpp; this fixture is never installed.
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/osal/path.hpp>
#include <iostream>

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        auto text = vinc::osal::path_to_utf8(std::filesystem::path(argv[index]));
        if (!text) {
            std::cerr << vinc::error_to_string(text.error()) << '\n';
            return 64;
        }

        arguments.push_back(std::move(*text));
    }

    return vinc::run_module_process(arguments);
}
#else
int main(int argc, char** argv) {
    return vinc::run_module_process(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
