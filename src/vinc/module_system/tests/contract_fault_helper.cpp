// Compile a private copy of the report writer so production has no fault hooks.
#ifdef VINC_FAULT_HOST
#include "../src/contract_report.cpp"
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#else
#include <thread>
#endif
#include <vinc/module/base_module.hpp>
#include <vinc/osal/path.hpp>
#include <fstream>

namespace {
enum class FaultMode { Delayed, Writing, Committed, Reentrant };
FaultMode mode = FaultMode::Delayed;
constexpr auto report_delay = std::chrono::milliseconds(250);
constexpr auto stalled_wait = std::chrono::milliseconds(10);

[[noreturn]] void stall() {
    for (;;) {
        std::this_thread::sleep_for(stalled_wait);
    }
}

class ContractFaultModule : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    ContractFaultModule() : BaseModule("fatal-worker") {
    }

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        if (args.empty()) {
            return get_module_name();
        }
        auto name = args.front();
        args.erase(args.begin());
        return name;
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (!args.empty()) {
            if (args[0] == "writing" || args[0] == "startup") {
                mode = FaultMode::Writing;
            } else if (args[0] == "committed") {
                mode = FaultMode::Committed;
            } else if (args[0] == "reentrant") {
                mode = FaultMode::Reentrant;
            }
        }
        if (args.size() > 1) {
            auto path = vinc::osal::path_from_utf8(args[1]);
            if (!path) {
                return std::unexpected(vinc::wrap_error(
                    vinc::module_system::ErrorCode::HookFailed,
                    vinc::module_system::ErrorOperation::StartHook, path.error()));
            }
            stop_marker_ = std::move(*path);
        }
        if (!args.empty() && args[0] == "startup") {
            vinc::contract_violation("startup writer stalled");
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args[0] == "fatal") {
            vinc::contract_index_violation("test fatal", 10, 3);
        }
        return 42;
    }

    vinc::Result<void> on_stop() override {
        if (!stop_marker_.empty()) {
            std::ofstream marker(stop_marker_);
            marker << "stop hook ran";
        }
        return {};
    }

private:
    std::filesystem::path stop_marker_;
};
}

#ifdef VINC_FAULT_HOST
namespace vinc::module_system::test {
void checkpoint(Point point, osal::Process*) {
    if (point == Point::ContractWriting) {
        if (mode == FaultMode::Writing) {
            stall();
        }
        if (mode == FaultMode::Reentrant) {
            // The nested call waits for its own outer writer. Central must end it.
            vinc::contract_violation("reentrant fatal");
        }
        if (mode == FaultMode::Delayed) {
            std::this_thread::sleep_for(report_delay);
        }
    }
    if (point == Point::ContractCommitted && mode == FaultMode::Committed) {
        stall();
    }
}
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const auto mode_index = vinc::detail::bootstrap::argument_count + 1;
    if (args.size() > mode_index) {
        const auto& selected = args[mode_index];
        if (selected == "writing" || selected == "startup") {
            mode = FaultMode::Writing;
        } else if (selected == "committed") {
            mode = FaultMode::Committed;
        } else if (selected == "reentrant") {
            mode = FaultMode::Reentrant;
        }
    }
    return vinc::run_module_process(args);
}
#else
REGISTER_MODULE(ContractFaultModule, "contractFault")
#endif
