// Framework-supplied child entry point. Forward launch arguments to the runtime
// so application modules only need a registered factory and BaseModule hooks.

#include <module/base_module.hpp>
#include <filesystem>
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        auto text = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(text.data()), text.size());
    }
    return vinc::run_module_process(args);
}
#else
int main(int argc, char** argv) {
    return vinc::run_module_process(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
