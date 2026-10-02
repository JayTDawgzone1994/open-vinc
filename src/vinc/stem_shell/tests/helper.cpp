// Shell test module with argument/completion probes and controlled blocking hooks.
// Its cleanup marker proves the central shutdown path reached the module lifecycle hook.

#include <module/base_module.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <cstdlib>

class ShellHelper : public vinc::BaseModule {
    std::filesystem::path directory_;
    std::string mode_;
    void mark(const std::string& suffix) { std::ofstream(directory_ / (get_module_name() + suffix)) << "done"; }
public:
    using BaseModule::BaseModule;
    std::string on_name(std::vector<std::string>& args) override {
        if (args.size() >= 2 && args[0] == "--name") {
            auto name = args[1]; args.erase(args.begin(), args.begin() + 2); return name;
        }
        return get_module_name();
    }
    int32_t on_start(const std::vector<std::string>& input) override {
        auto args = input;
        if (!args.empty() && args[0] == "shellHelper") args.erase(args.begin());
        if (args.size() != 2) return 2;
        mode_ = args[0]; directory_ = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(args[1].data()), args[1].size()));
        if (mode_ == "output") {
            std::cout << "startup-output\n" << std::flush;
            std::cerr << "startup-error\n";
        }
        mark(".ready"); return 0;
    }
    int32_t module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args[0] == get_module_name()) args.erase(args.begin());
        if (args.empty()) return 0;
        if (args[0] == "output") {
            std::cout << "command-output\x1b[2J\n" << std::flush;
            std::cerr << "command-error\n";
            mark(".output");
        }
        if (args[0] == "crash-output") { std::cout << "crash-output\n" << std::flush; std::_Exit(42); }
        if (args[0] == "hang") { mark(".working"); std::this_thread::sleep_for(std::chrono::minutes(2)); }
        if (args[0] == "slow") { mark(".working"); std::this_thread::sleep_for(std::chrono::milliseconds(350)); }
        if (args[0] == "fail") { mark(".failed-command"); return 17; }
        if (args[0] == "check") {
            if (args.size() != 4 || args[1] != "中文 空白" || !args[2].empty() || args[3] != "D:\\Some Folder\\a.txt") return 18;
            mark(".checked");
        }
        return 0;
    }
    std::vector<std::string> module_autoComplete(std::vector<std::string>) override { return {"alpha", "beta"}; }
    void on_stop() override {
        if (mode_ == "slow-stop") std::this_thread::sleep_for(std::chrono::milliseconds(500));
        mark(".stopped");
        if (mode_ == "output") std::cout << "stop-tail-without-newline" << std::flush;
    }
};
REGISTER_MODULE(ShellHelper, "shellHelper")
