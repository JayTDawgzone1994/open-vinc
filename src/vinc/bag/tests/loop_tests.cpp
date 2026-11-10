// Compile the actual Recorder loop; only Host scheduling is controlled here.
#include "../module/recorder_module.cpp"
#include <vinc/module/detail/context_runtime.hpp>
#include <random>
#include <stdexcept>

namespace vinc::bag::test {
Result<void> run(std::unique_ptr<vinc::detail::ModuleObject>, vinc::detail::ContextRuntime&);
}

namespace {
using namespace std::chrono_literals;
constexpr auto future_discovery = 30s;
constexpr std::size_t maximum_test_rounds = 8;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void checked(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

struct RecorderTestAccess {
    static void recording(RecorderModule& module, const std::filesystem::path& path, Deadline discovery) {
        auto created = Recorder::create(path);
        if (!created) {
            throw std::runtime_error(error_to_string(created.error()));
        }

        module.recorder_ = std::move(*created);
        module.next_discovery_ = discovery;
    }
    static void finish(RecorderModule& module) {
        module.finish();
    }
    static void fail(RecorderModule& module) {
        module.callback_failure_ = Error{bag::ErrorCode::StorageFailed, bag::ErrorOperation::Enqueue};
    }
    static void discovery_due(RecorderModule& module) {
        module.next_discovery_ = Deadline{};
    }
    static bool failure_handled(RecorderModule& module) {
        return module.failure_reported_ && !module.recorder_->status().accepting;
    }
};

class TestRuntime final : public vinc::detail::ContextRuntime {
public:
    bool alive = true;
    std::size_t rounds = 0;
    std::size_t waits = 0;
    std::function<std::uint64_t()> command_action;
    std::function<void(Deadline)> wait_action;

    bool living() const noexcept override {
        return alive;
    }
    std::string_view module_name() const noexcept override {
        return "recorder";
    }
    Result<ModulePage> list_modules(MID, Deadline) override {
        return ModulePage{};
    }
    Result<std::uint64_t> commands() override {
        check(++rounds <= maximum_test_rounds, "loop failed to wait/stop");
        return command_action();
    }
    void wait(Deadline deadline) override {
        ++waits;
        wait_action(deadline);
        alive = false;
    }
    void wake() override {}
    void fail(const Error&) override {
        alive = false;
    }
    Result<std::unique_ptr<vinc::detail::ContextLock>> lock(std::uint64_t, Deadline) override {
        return std::unexpected(Error{module_system::ErrorCode::ResourcesUnavailable,
                                     module_system::ErrorOperation::AccessLock});
    }
};

void busy_then_idle() {
    auto owned = std::make_unique<RecorderModule>();
    auto& module = *owned;
    TestRuntime runtime;
    runtime.command_action = [&] { return runtime.rounds < 3 ? 1 : 0; };
    runtime.wait_action = [&](Deadline deadline) {
        check(runtime.rounds == 3, "busy batches must not enter wait");
        check(deadline == Deadline::max(), "idle Recorder uses Host fallback");
    };
    checked(bag::test::run(std::move(owned), runtime));
    check(runtime.waits == 1, "empty batch must enter wait");
}

void stopped_batch() {
    auto owned = std::make_unique<RecorderModule>();
    auto& module = *owned;
    TestRuntime runtime;
    runtime.command_action = [&] {
        runtime.alive = false;
        return 0;
    };
    runtime.wait_action = [](Deadline) { check(false, "stopped Recorder must not wait"); };
    checked(bag::test::run(std::move(owned), runtime));
}

void discovery_deadline(const std::filesystem::path& path, bool finished) {
    auto owned = std::make_unique<RecorderModule>();
    auto& module = *owned;
    const auto deadline = std::chrono::steady_clock::now() + future_discovery;
    RecorderTestAccess::recording(module, path, finished ? Deadline{} : deadline);
    if (finished) {
        RecorderTestAccess::finish(module);
    }
    TestRuntime runtime;
    runtime.command_action = [] { return 0; };
    runtime.wait_action = [&](Deadline actual) {
        check(actual == (finished ? Deadline::max() : deadline),
              "only active recording uses discovery deadline");
    };
    checked(bag::test::run(std::move(owned), runtime));
    check(runtime.waits == 1, "recording should wait once");
}

void busy_maintenance(const std::filesystem::path& path, bool discovery_due) {
    auto owned = std::make_unique<RecorderModule>();
    auto& module = *owned;
    RecorderTestAccess::recording(module, path, std::chrono::steady_clock::now() + future_discovery);
    TestRuntime runtime;
    runtime.command_action = [&] {
        if (runtime.rounds == 1) {
            if (discovery_due) {
                // This isolated Host deliberately has no Bus client. Reaching
                // discovery must report that failure instead of starving it.
                RecorderTestAccess::discovery_due(module);
            } else {
                RecorderTestAccess::fail(module);
            }
        } else {
            check(RecorderTestAccess::failure_handled(module), "busy loop must check recording failure");
            runtime.alive = false;
        }
        return 1;
    };
    runtime.wait_action = [](Deadline) { check(false, "busy maintenance must not wait"); };
    auto result = bag::test::run(std::move(owned), runtime);
    const auto expected = discovery_due ? bag::ErrorCode::BusFailed : bag::ErrorCode::StorageFailed;
    check(!result && result.error().is(expected), "maintenance failure survives normal cleanup");
}
}

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() /
            ("vinc-recorder-loop-" + std::to_string(std::random_device{}()));
        check(std::filesystem::create_directory(root), "exclusive test directory");
        busy_then_idle();
        stopped_batch();
        discovery_deadline(root / "active", false);
        discovery_deadline(root / "finished", true);
        busy_maintenance(root / "failure", false);
        busy_maintenance(root / "discovery", true);
        check(std::filesystem::weakly_canonical(root).parent_path() ==
              std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()), "cleanup boundary");
        std::filesystem::remove_all(root);
        std::cout << "Recorder wait policy passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
