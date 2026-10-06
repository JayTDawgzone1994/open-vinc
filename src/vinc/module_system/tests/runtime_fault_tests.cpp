// Compile the actual lifecycle dispatcher with test-only fault checkpoints.
#include "../src/runtime/module_runtime.cpp"
#include <stdexcept>

namespace {
int reports = 0;
bool fail_ready = false;
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
class LifecycleFixture final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    LifecycleFixture() : BaseModule("fixture") {}
    bool fail_start = true;
    int starts = 0;
    int stops = 0;
    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
    vinc::Result<void> on_start(const std::vector<std::string>&) override {
        ++starts;
        if (fail_start) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }
        return {};
    }
    vinc::Result<void> on_stop() override {
        ++stops;
        return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                           vinc::module_system::ErrorOperation::StopHook});
    }
};
}
namespace vinc::module_system::test {
void checkpoint(Point point, osal::Process*) {
    if (point == Point::ReportFailure) {
        ++reports;
        throw std::bad_alloc();
    }
    if (point == Point::Ready && fail_ready) {
        throw std::bad_alloc();
    }
}
}
int main() {
    using namespace vinc;
    try {
        RuntimeInbox inbox;
        std::vector<std::string> args;
        LifecycleFixture rejected;
        auto result = run_started_hooks(rejected, args, {}, inbox);
        check(!result && result.error().is(module_system::ErrorCode::HookFailed), "preserve primary hook failure");
        check(rejected.starts == 1 && rejected.stops == 1, "report failure cannot bypass stop hook");
        check(reports == 2, "primary and cleanup report attempts remain separate");
        LifecycleFixture started;
        started.fail_start = false;
        fail_ready = true;
        auto allocation = run_started_hooks(started, args, {}, inbox);
        check(!allocation && allocation.error().is(module_system::ErrorCode::ResourceExhausted), "framework allocation failure retained");
        check(started.starts == 1 && started.stops == 1, "framework failure still stops exactly once");
        check(reports == 4, "both reports attempted after framework failure");
        std::cout << "runtime fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
