// Test-only BaseModule with controlled startup, command and stop behaviors.
// Intentional hangs/crashes let the parent test the framework's supervision boundaries.

#include <module/base_module.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#include <cstdlib>
#include <stdexcept>

class TestModule final : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;
    int32_t on_start(const std::vector<std::string>& args) override {
        if (!args.empty() && args[0] == "init-hang") std::this_thread::sleep_for(std::chrono::minutes(2));
        if (!args.empty() && args[0] == "fail") return 12;
        if (!args.empty() && args[0] == "crash-start") std::_Exit(42);
        if (args.size() == 2 && args[0] == "marker") marker = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(args[1].data()), args[1].size()));
        return 0;
    }
    int32_t node_interface(std::vector<std::string> args) override {
        if (args.empty()) return 42;
        if (args[0] == "crash") std::_Exit(42);
        if (args[0] == "hang") std::this_thread::sleep_for(std::chrono::minutes(2));
        if (args[0] == "throw") throw std::runtime_error("test hook");
        if (args[0] == "name") return args.size() == 2 && args[1] == get_node_name() ? 0 : 1;
        if (args[0] == "unicode") return args.size() == 4 && args[1] == "模組測試" && args[2] == "a b\"c" && args[3].empty() ? 0 : 1;
        return 42;
    }
    std::vector<std::string> node_autoComplete(std::vector<std::string>) override { return {"hello", "status"}; }
    void on_stop() override { if (!marker.empty()) { std::ofstream out(marker); out << get_node_name(); } }
private:
    std::filesystem::path marker;
};
REGISTER_MODULE(TestModule, "helper")
