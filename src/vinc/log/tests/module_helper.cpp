#include <vinc/module/base_module.hpp>
#include <cstdio>
#include <thread>

namespace {
using namespace vinc;

class LogModule final : public BaseModule<RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;

    Result<void> on_start(const std::vector<std::string>&) override {
        std::printf("raw-start \xe4\xb8\xad\xe6\x96\x87\n");
        std::fflush(stdout);
        return context.log.info("structured-start {}", 42);
    }

    Result<void> on_stop() override {
        std::fprintf(stderr, "raw-stop\n");
        std::fflush(stderr);
        return context.log.warn("structured-stop");
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args[0] == "flood") {
            constexpr std::size_t attempts = 10000;
            std::int32_t rejected = 0;
            for (std::size_t i = 0; i < attempts; ++i) {
                if (!context.log.info("flood {} {}", i, std::string(1024, 'x'))) {
                    ++rejected;
                }
            }
            return rejected;
        }
        if (!args.empty() && args[0] == "barrier") {
            (void)context.log.info("after-flood-marker");
            return 0;
        }
        if (!args.empty() && args[0] == "debug") {
            auto result = context.log.debug("debug-enabled");
            if (!result) {
                return std::unexpected(result.error());
            }
            return context.log.enabled(log::Level::Debug) ? 1 : 0;
        }
        return 7;
    }
};
}
REGISTER_MODULE(LogModule, "logTest")
