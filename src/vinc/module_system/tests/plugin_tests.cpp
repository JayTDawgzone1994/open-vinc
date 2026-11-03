#include "plugin_test_support.hpp"
#include <vinc/module/host/module_manager.hpp>
#include <vinc/module/detail/plugin.hpp>
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/osal/dynamic_library.hpp>
#include <vinc/osal/path.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
using vinc::test::plugin::check;
using vinc::test::plugin::take;
using vinc::test::plugin::Scope;
constexpr auto test_timeout = vinc::test::plugin::wait_budget;
constexpr auto poll_interval = vinc::test::plugin::poll_interval;
// Classification has a setup budget, not a startup latency requirement.
constexpr auto classification_startup_budget = 5s;
constexpr auto stalled_startup_budget = 600ms;
constexpr int expected_cleanup_faults = 1;
bool probe_confirmed_shutdown = false;

void test_library(const std::filesystem::path& path) {
    auto invalid = osal::DynamicLibrary::open("relative.dll");
    check(!invalid && invalid.error().is(osal::ErrorCode::InvalidArgument), "relative library rejected");
    auto library = take(osal::DynamicLibrary::open(path));
    auto missing = library.function("not_an_export");
    check(!missing && missing.error().is(osal::ErrorCode::NotFound), "missing export rejected");
    auto moved = std::move(library);
    check(!library.function(detail::plugin_entry_symbol), "moved owner is empty");
    auto symbol = take(moved.function(detail::plugin_entry_symbol));
    // Test the documented same-build Plugin C export signature, not arbitrary data symbols.
    const auto query = reinterpret_cast<detail::PluginQuery>(symbol);
    check(query()->version == detail::plugin_entry_version, "loaded function is callable");
    check(bool(moved.close()) && bool(moved.close()), "close is idempotent");
    check(!moved.function(detail::plugin_entry_symbol), "closed owner cannot return symbols");
}

void expect_failure(const std::filesystem::path& path, module_system::ErrorCode code,
                    module_system::ErrorOperation operation, std::chrono::milliseconds startup_budget,
                    std::string type, bool require_safe_exit, std::optional<ErrorRecord> expected_cause) {
    Scope scope;
    scope.create();
    auto& manager = scope.manager;
    ModulePolicy policy;
    policy.startup_timeout = startup_budget;
    MID mid = 0;
    std::optional<Error> start_error;
    const auto began = control::Clock::now();
    try {
        check(bool(manager->register_module_type({type, path, policy})), "register test plugin");
        mid = take(manager->start_module(type));
        auto started = manager->wait_running(mid, control::Clock::now() + test_timeout);
        if (!started) {
            start_error = started.error();
        }
        check(!started && started.error().is(code) && started.error().operation == static_cast<std::int32_t>(operation),
              "wait_running returns the expected failure");
        const auto deadline = control::Clock::now() + test_timeout;
        for (;;) {
            auto inspected = manager->inspect(mid);
            check(inspected.has_value(), "failed Module remains inspectable");
            const auto& snapshot = *inspected;
            if (snapshot.last_exit) {
                check(snapshot.mid == mid && snapshot.last_failure && snapshot.last_failure->mid == mid &&
                      snapshot.last_failure->error.is(code) &&
                      snapshot.last_failure->error.operation == static_cast<std::int32_t>(operation),
                      "failure identity retained");
                if (require_safe_exit) {
                    check(snapshot.last_exit->kind == osal::ExitKind::Code &&
                          snapshot.last_exit->value == detail::module_application_failure_exit,
                          "rejected Plugin was not unloaded or destructed before Host exit");
                }
                if (expected_cause) {
                    bool found = false;
                    for (const auto& cause : snapshot.last_failure->error.causes()) {
                        found = found || cause == *expected_cause;
                    }
                    check(found, "expected cause survives Module failure reporting");
                }
                check(scope.finish(), "failed Host shutdown and test cleanup complete");
                return;
            }
            check(control::Clock::now() < deadline, "failed Host is reaped");
            std::this_thread::sleep_for(poll_interval);
        }
    } catch (...) {
        std::cerr << "Plugin case=" << path << " type=" << type << " MID=" << mid
                  << " expected=" << error_to_string(Error{code, operation})
                  << " startup_ms=" << startup_budget.count()
                  << " wait_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout).count()
                  << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(control::Clock::now() - began).count()
                  << " wait_running=" << (start_error ? error_to_string(*start_error) : "no error result") << '\n';
        if (manager) {
            if (auto snapshot = manager->inspect(mid)) {
                std::cerr << "state=" << static_cast<int>(snapshot->state)
                          << " primary=" << (snapshot->last_failure ? error_to_string(snapshot->last_failure->error) : "none")
                          << " cleanup=" << (snapshot->cleanup_failure ? error_to_string(snapshot->cleanup_failure->error) : "none") << '\n';
                if (snapshot->last_exit) {
                    std::cerr << "exit_kind=" << static_cast<int>(snapshot->last_exit->kind)
                              << " exit_value=" << snapshot->last_exit->value << '\n';
                }
            }
        }
        throw;
    }
}

void expect_rejection(const std::filesystem::path& path, module_system::ErrorCode code,
                      module_system::ErrorOperation operation = module_system::ErrorOperation::LoadPlugin,
                      std::string type = "pluginFixture", bool safe_exit = false,
                      std::optional<ErrorRecord> cause = {}) {
    expect_failure(path, code, operation, classification_startup_budget, std::move(type), safe_exit, cause);
}

void expect_stalled_loader(const std::filesystem::path& path) {
    expect_failure(path, module_system::ErrorCode::StartupTimeout, module_system::ErrorOperation::Ready,
                   stalled_startup_budget, "pluginFixture", false, {});
}

void test_assertion_cleanup(const std::filesystem::path& valid) {
    vinc::test::plugin::CleanupEvidence evidence;
    std::filesystem::path directory;
    MID mid = 0;
    bool caught = false;
    try {
        Scope scope(&evidence);
        directory = scope.directory;
        const auto copy = directory / valid.filename();
        evidence.marker = directory / "marker";
        std::filesystem::copy_file(valid, copy);
        scope.create();
        check(bool(scope.manager->register_module_type({"pluginFixture", copy})), "cleanup fixture registration");
        mid = take(scope.manager->start_module("pluginFixture", {take(osal::path_to_utf8(evidence.marker))}));
        check(bool(scope.manager->wait_running(mid, control::Clock::now() + test_timeout)), "cleanup fixture ready");
        check(false, "injected assertion after Host readiness");
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "injected assertion after Host readiness";
    }
    check(caught, "intended assertion reached");
    check(evidence.shutdown_confirmed && evidence.files_removed && !std::filesystem::exists(directory),
          "assertion cleanup reaps before deleting files");
    check(evidence.snapshots.size() == 1 && evidence.snapshots.front().mid == mid &&
          evidence.snapshots.front().last_exit.has_value(), "assertion cleanup records actual Host exit");
    check(evidence.marker_after_shutdown == "destroyed in plugin",
          "assertion cleanup keeps marker directory until Plugin destructor finishes");
}

Result<void> report_uncertain_shutdown(ModuleManager& manager, Deadline deadline) {
    auto stopped = manager.wait_shutdown(deadline);
    if (!stopped) {
        return stopped;
    }
    // Inject an error visible only to Scope, after the probe independently
    // confirms real shutdown. This lets the test safely remove retained files.
    probe_confirmed_shutdown = true;
    return std::unexpected(Error{module_system::ErrorCode::Timeout, module_system::ErrorOperation::WaitShutdown});
}

void test_cleanup_retention(const std::filesystem::path& valid) {
    vinc::test::plugin::CleanupEvidence evidence;
    const auto failures_before = vinc::test::plugin::cleanup_failures;
    Scope scope(&evidence, report_uncertain_shutdown);
    const auto copy = scope.directory / valid.filename();
    std::filesystem::copy_file(valid, copy);
    evidence.marker = scope.directory / "marker";
    scope.create();
    check(bool(scope.manager->register_module_type({"pluginFixture", copy})), "retention fixture registration");
    const auto mid = take(scope.manager->start_module("pluginFixture", {take(osal::path_to_utf8(evidence.marker))}));
    check(bool(scope.manager->wait_running(mid, control::Clock::now() + test_timeout)), "retention fixture ready");
    std::cout << "Injecting expected shutdown diagnostic to verify file retention\n";
    check(!scope.finish(), "uncertain shutdown must fail cleanup");
    check(probe_confirmed_shutdown && !evidence.shutdown_confirmed && !evidence.files_removed,
          "Scope cannot claim confirmation from an error result");
    check(vinc::test::plugin::cleanup_failures == failures_before + expected_cleanup_faults &&
          std::filesystem::exists(copy) && std::filesystem::exists(evidence.marker),
          "uncertain cleanup retains Plugin and marker");
    // The independent probe established confirmed shutdown, not a guessed PID check.
    std::filesystem::remove_all(scope.directory);
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 9, "eight plugin fixture paths required");
        const auto valid = std::filesystem::absolute(argv[1]);
        test_library(valid);
        expect_rejection(valid.parent_path() / "missing-plugin.dll", module_system::ErrorCode::PluginLoadFailed);
        expect_rejection(std::filesystem::absolute(argv[2]), module_system::ErrorCode::InvalidPlugin);
        expect_rejection(std::filesystem::absolute(argv[3]), module_system::ErrorCode::IncompatiblePlugin);
        expect_rejection(std::filesystem::absolute(argv[4]), module_system::ErrorCode::HookFailed,
                       module_system::ErrorOperation::ConstructModule, "pluginFixture", false,
                       Error{module_system::ErrorCode::InvalidFactory, module_system::ErrorOperation::ConstructModule}.record());
        expect_rejection(std::filesystem::absolute(argv[5]), module_system::ErrorCode::ServiceFailed,
                       module_system::ErrorOperation::InitializePlugin, "pluginFixture", true);
        expect_stalled_loader(std::filesystem::absolute(argv[6]));
        expect_rejection(valid, module_system::ErrorCode::InvalidPlugin, module_system::ErrorOperation::LoadPlugin, "wrongType");
        expect_rejection(std::filesystem::absolute(argv[7]), module_system::ErrorCode::HookException, module_system::ErrorOperation::ConstructModule);
        expect_rejection(std::filesystem::absolute(argv[8]), module_system::ErrorCode::ServiceFailed,
                       module_system::ErrorOperation::InitializePlugin, "pluginFixture", true,
                       Error{osal::ErrorCode::CoordinatorInUse, osal::ErrorOperation::BindCoordinator}.record());

        Scope missing_scope;
        missing_scope.create(missing_scope.directory / "missing-host.exe");
        auto& missing_host = missing_scope.manager;
        check(bool(missing_host->register_module_type({"pluginFixture", valid})),
              "register with missing Host");
        const auto missing_mid = take(missing_host->start_module("pluginFixture"));
        auto missing_start = missing_host->wait_running(missing_mid, control::Clock::now() + test_timeout);
        check(!missing_start && missing_start.error().is(module_system::ErrorCode::SpawnFailed),
              "Host spawn failure differs from Plugin load failure");

        check(missing_scope.finish(), "missing Host cleanup");
        Scope scope;
        scope.create();
        auto& manager = scope.manager;
        const auto relocated = scope.directory / std::filesystem::path(u8"Plugin 中文 空白.dll");
        std::filesystem::copy_file(valid, relocated, std::filesystem::copy_options::overwrite_existing);
        check(bool(manager->register_module_type({"pluginFixture", relocated})), "valid registration");
        const auto marker = scope.directory / "plugin-destroyed.txt";
        std::filesystem::remove(marker);
        const auto mid = take(manager->start_module("pluginFixture", {take(osal::path_to_utf8(marker))}));
        check(bool(manager->wait_running(mid, control::Clock::now() + test_timeout)), "valid plugin ready");
        check(take(manager->command(mid, {}, control::Clock::now() + test_timeout)) == 91, "plugin virtual hook");
        check(bool(manager->wait_shutdown(control::Clock::now() + test_timeout)), "Host cleanup completes");
        std::ifstream input(marker);
        std::string contents;
        std::getline(input, contents);
        check(contents == "destroyed in plugin", "Plugin paired destroy ran before exit");
        input.close();
        check(scope.finish(), "valid Plugin cleanup");
        test_assertion_cleanup(valid);
        test_cleanup_retention(valid);
        check(vinc::test::plugin::cleanup_failures == expected_cleanup_faults, "no unexpected scope cleanup failures");
        std::cout << "plugin loader tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
