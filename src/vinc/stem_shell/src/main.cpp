// Central executable entry point: parse -S, locate Tcl resources, reserve command names,
// register compiled module definitions, then enter the Shell/deployment loop.

#include <stem_shell/stem_shell.hpp>
#include <stem_shell/tcl_runtime.hpp>
#include <module_catalog.hpp>
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int application(int argc, char** argv) {
    try {
        std::filesystem::path script;
        std::vector<std::string> script_args;
        if (argc > 1) {
            if (std::string_view(argv[1]) == "--help" && argc == 2) {
                std::cout << "Usage: Vinc [-S file.tcl [args...]]\n"; return 0;
            }
            if (std::string_view(argv[1]) != "-S" || argc < 3 || !*argv[2]) {
                std::cerr << "Usage: Vinc [-S file.tcl [args...]]\n"; return 2;
            }
            script = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(argv[2])));
            script_args.assign(argv + 3, argv + argc);
        }
        vinc::shell::initialize_tcl(argv[0]);
        vinc::ModuleManager manager;
        // Reserve Shell and Tcl names before registering factories, so collisions fail
        // at registration instead of making a module command silently unreachable.
        vinc::StemShell shell(manager, std::cout);
        vinc::register_built_modules(manager);
        shell.run(std::move(script), std::move(script_args)); return 0;
    } catch (const std::exception& e) {
        std::cerr << "Shell startup failed: " << e.what() << '\n'; return 1;
    }
}
#ifdef _WIN32
// Windows supplies UTF-16 arguments. Convert once to the UTF-8 contract used by
// the framework and Tcl, including non-ASCII script paths and arguments.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!length) return 2;
        std::string value(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length, nullptr, nullptr);
        value.pop_back(); args.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    for (auto& arg : args) pointers.push_back(arg.data());
    return application(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return application(argc, argv); }
#endif
