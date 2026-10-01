#include <stem_shell/stem_shell.hpp>
#include <module_catalog.hpp>
#include <iostream>

int main(int argc, char** argv) {
    try {
        std::vector<std::filesystem::path> scripts;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) != "-sc" || i + 1 >= argc) {
                std::cerr << "Usage: stem_shell [-sc file]...\n"; return 2;
            }
            scripts.emplace_back(argv[++i]);
        }
        vinc::ModuleManager manager;
        vinc::register_built_modules(manager);
        vinc::StemShell shell(manager, std::cout);
        for (const auto& path : scripts) {
            if (shell.run_script(path)) { shell.wait_shutdown(); return 1; }
        }
        shell.run(); return 0;
    } catch (const std::exception& e) {
        std::cerr << "Shell startup failed: " << e.what() << '\n'; return 1;
    }
}
