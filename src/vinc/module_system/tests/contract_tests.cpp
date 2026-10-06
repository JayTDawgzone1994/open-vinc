// Real process isolation and retained diagnostics, plus hostile/incomplete record decoding.
#include <vinc/module/host/module_manager.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/osal/path.hpp>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;
namespace {
constexpr auto test_deadline = 8s;
constexpr auto observation_interval = 10ms;
constexpr std::uint64_t fixture_mid = 123;
// Independent protocol fixture: native state, then LE version/MID/flags/line/index/size.
constexpr std::size_t fixture_bytes = 56 + 256 + 768 + 256;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

std::string unique_name() {
    return "faulttest_" + std::to_string(std::random_device{}());
}

template<class Predicate> void await(Predicate predicate, const char* message) {
    const auto end = control::Clock::now() + test_deadline;
    while (!predicate() && control::Clock::now() < end) {
        std::this_thread::sleep_for(observation_interval);
    }
    check(predicate(), message);
}

void test_validation() {
    const auto name = unique_name();
    auto owner = take(detail::ContractReportStorage::create(name, fixture_mid));
    check(!take(owner->read_after_exit()), "empty record");
    auto duplicate = detail::ContractReportStorage::create(name, fixture_mid);
    check(!duplicate, "exclusive creation");
    auto opened = take(detail::ContractReportStorage::open(name, fixture_mid));
    auto wrong_mid = detail::ContractReportStorage::open(name, fixture_mid + 1);
    check(!wrong_mid, "bootstrap rejects wrong MID");
    opened.reset();
    check(bool(detail::ContractReportStorage::open(name, fixture_mid)), "child close preserves parent name");

    auto memory = take(osal::SharedMemory::open(name, osal::SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(fixture_bytes, osal::SharedMemoryAccess::ReadWrite));
    auto bytes = take(mapping.writable_bytes());
    // Native IPC atomic boundary: creator constructed SharedWord at page-aligned offset zero.
    auto& state = *reinterpret_cast<osal::SharedWord*>(bytes.data());
    osal::shared_store(state, 1);
    auto partial = owner->read_after_exit();
    check(!partial && partial.error().is(module_system::ErrorCode::IncompleteFaultReport), "partial report");
    osal::shared_store(state, 99);
    auto unknown = owner->read_after_exit();
    check(!unknown && unknown.error().is(module_system::ErrorCode::InvalidFaultReport), "unknown state");
    osal::shared_store(state, 2);
    bytes[8] = std::byte{99};
    check(!owner->read_after_exit(), "bad version");
    bytes[8] = std::byte{1};
    bytes[16] = std::byte{0};
    check(!owner->read_after_exit(), "bad MID");
    bytes[16] = static_cast<std::byte>(fixture_mid);
    bytes[24] = std::byte{4};
    check(!owner->read_after_exit(), "unknown flags");
    bytes[24] = std::byte{0};
    std::fill(bytes.begin() + 56, bytes.begin() + 56 + 256, std::byte{'x'});
    check(!owner->read_after_exit(), "unterminated text rejected");
    bytes[56] = std::byte{0};
    check(bool(owner->read_after_exit()), "valid bounded record");

    check(bool(mapping.unmap()) && bool(memory.close()) && bool(owner->close()), "explicit cleanup");
    check(!osal::SharedMemory::open(name, osal::SharedMemoryAccess::ReadWrite), "name reclaimed");
}

void test_modules(const std::filesystem::path& helper) {
    auto manager = take(ModuleManager::create());
    ModulePolicy policy;
    policy.startup_timeout = test_deadline;
    check(bool(manager->register_module_type({"helper", helper, "worker", policy})), "register helper");
    const auto survivor = take(manager->start_module("helper", {"--name", "survivor"}));
    check(bool(manager->wait_running(survivor, control::Clock::now() + test_deadline)), "survivor ready");
    const auto marker = std::filesystem::temp_directory_path() / unique_name();
    const auto marker_text = take(osal::path_to_utf8(marker));
    for (const auto* command : {"contract", "contract-stdio", "contract-concurrent"}) {
        const auto mid = take(manager->start_module("helper", {"--name", command, "marker", marker_text}));
        check(bool(manager->wait_running(mid, control::Clock::now() + test_deadline)), "fault target ready");
        const auto pid = manager->inspect(mid)->pid;
        auto result = manager->command(mid, {command}, control::Clock::now() + test_deadline);
        check(!result, "fatal command cannot succeed");
        await([&] { return manager->inspect(mid)->state == ModuleState::Failed; }, "fault module reaped");
        const auto snapshot = *manager->inspect(mid);
        check(snapshot.last_contract_failure.has_value(), "retained contract diagnostic");
        const auto& diagnostic = *snapshot.last_contract_failure;
        check(diagnostic.mid == mid && diagnostic.pid == pid, "trusted identity");
        check(diagnostic.report.has_value(), "complete report after child exit");
        const auto& report = *diagnostic.report;
        check(report.has_index && report.index == 10 && report.size == 3, "index and size");
        check(std::string(report.reason.data()) == "array index out of range", "contract reason");
        check(std::string(report.file.data()).find("storage.hpp") != std::string::npos && report.line > 0,
              "detection location");
        check(diagnostic.exit.kind == osal::ExitKind::Code && diagnostic.exit.value == detail::contract_exit_status,
              "runtime exits without waiting for stdio or hooks");
        check(!std::filesystem::exists(marker), "fatal path skips user on_stop");
        check(take(manager->command(survivor, {}, control::Clock::now() + test_deadline)) == 42,
              "other module remains functional");
    }

    auto restart_manager = take(ModuleManager::create());
    policy.max_restarts = 1;
    policy.restart_delay = 100ms;
    check(bool(restart_manager->register_module_type({"helper", helper, "restartFault", policy})), "restart policy");
    const auto original_mid = take(restart_manager->start_module("helper"));
    check(bool(restart_manager->wait_running(original_mid, control::Clock::now() + test_deadline)), "restart ready");
    (void)restart_manager->command(original_mid, {"contract"}, control::Clock::now() + test_deadline);
    await([&] {
        auto snapshot = restart_manager->inspect(std::string("restartFault"));
        return snapshot && snapshot->mid != original_mid && snapshot->state == ModuleState::Running;
    }, "fresh generation starts");
    const auto restarted = *restart_manager->inspect(std::string("restartFault"));
    check(restarted.last_contract_failure && restarted.last_contract_failure->mid == original_mid,
          "previous diagnostic survives restart");
    check(take(restart_manager->command(restarted.mid, {}, control::Clock::now() + test_deadline)) == 42,
          "new generation does not inherit old fatal state");

    const auto startup = take(manager->start_module("helper", {"--name", "startupFault", "contract-start"}));
    await([&] { return manager->inspect(startup)->state == ModuleState::Failed; }, "startup contract exits");
    check(manager->inspect(startup)->last_contract_failure.has_value(), "binding precedes startup hook");
    restart_manager->begin_shutdown();
    check(bool(restart_manager->wait_shutdown(control::Clock::now() + test_deadline)), "restart manager shutdown");

    manager->begin_shutdown();
    check(bool(manager->wait_shutdown(control::Clock::now() + test_deadline)), "all resources reaped");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected helper path");
        test_validation();
        test_modules(argv[1]);
        std::cout << "contract record validation, stdio isolation and restart tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
