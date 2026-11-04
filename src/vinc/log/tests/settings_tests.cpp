#include "support.hpp"
#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/log_settings.hpp>
#include <vinc/module/detail/instance.hpp>
#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/log/detail/manager.hpp>
#include <vinc/shell/detail/session_feedback.hpp>
#include <vinc/shell/detail/command_router.hpp>
#include <fstream>
#include <iostream>

namespace vinc::param::detail {
struct ParamManagerTestAccess {
    static void close_admission(ParamManager& manager) {
        std::lock_guard lock(manager.mutex_);
        manager.closing_ = true;
    }
};
}

namespace {
using namespace vinc;
using namespace log_test;
using Access = detail::ManagerAccess;
constexpr std::uint64_t small_file_bytes = 2048;
constexpr std::uint64_t small_total_bytes = 4096;

std::string read_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void settings_and_persistence() {
    Directory directory;
    const auto name = test::instance_name();
    auto manager = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*manager, directory.path));
    auto active = take(Access::configure_log_from_params(*manager, directory.path));
    check(active.enabled && active.max_file_bytes == config::log_file_bytes &&
          active.max_total_bytes == config::log_total_bytes, "registration defaults");
    const auto entries = take(Access::read_params(*manager,
        {"vinc.log_file_enabled", "vinc.log_file_bytes", "vinc.log_total_bytes"}));
    for (const auto& [key, entry] : entries) {
        check(entry.permission == param::Permission::Limited &&
              entry.restart_hint == param::RestartHint::App, "Limited/App contract");
    }

    shell::CommandRouter router(*manager);
    take(router.execute({"param", "set", "vinc.log_file_bytes", std::to_string(small_file_bytes)}));
    take(router.execute({"param", "set", "vinc.log_total_bytes", std::to_string(small_total_bytes)}));
    const auto repaired = take(router.execute({"param", "set", "vinc.log_file_bytes", "8192"}));
    check(repaired.message.find("restored file_bytes=2048, total_bytes=4096") != std::string::npos,
          "invalid pair restores last validated pair, not active startup defaults");
    check(!router.execute({"param", "set", "vinc.log_file_bytes", "-1"}), "uint64 rejects negative");
    check(!router.execute({"param", "set", "vinc.log_file_enabled", "not-bool"}), "typed bool validation");
    take(router.execute({"param", "set", "vinc.log_file_enabled", "false"}));
    const auto status = take(Access::log_command(*manager, {"status"}));
    check(status.find("file=enabled") != std::string::npos &&
          status.find("file_bytes=" + std::to_string(config::log_file_bytes)) != std::string::npos,
          "edits leave active policy and capacity unchanged");
    take(Access::log_command(*manager, {"reopen"}));
    check(take(Access::log_command(*manager, {"status"})).find(
              "file_bytes=" + std::to_string(config::log_file_bytes)) != std::string::npos,
          "reopen keeps applied capacity");
    take(Access::param_command(*manager, {"save"}));
    take(router.execute({"param", "set", "vinc.log_file_enabled", "true"}));
    manager.reset();

    const auto logs = directory.path / name / "logs";
    const auto marker = logs / "preserve.txt";
    {
        std::ofstream output(marker);
        output << "untouched";
    }
    const auto marker_time = std::filesystem::last_write_time(marker);
    auto restarted = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*restarted, directory.path));
    const auto disabled = take(Access::configure_log_from_params(*restarted, directory.path));
    check(!disabled.enabled && disabled.max_file_bytes == small_file_bytes &&
          disabled.max_total_bytes == small_total_bytes, "saved settings adopted, unsaved edit ignored");
    for (std::size_t index = 0; index < config::log_queue_records + 1; ++index) {
        take(Access::log_diagnostic(*restarted, "disabled-display-record"));
    }
    log::View view;
    view.enabled = true;
    check(Access::log_take(*restarted, view).find("disabled-display-record") != std::string::npos,
          "bounded display remains available while files disabled");
    const auto disabled_status = take(Access::log_command(*restarted, {"status"}));
    check(disabled_status.find("file=disabled") != std::string::npos &&
          disabled_status.find("queued=0 dropped=0") != std::string::npos &&
          disabled_status.find("file_lost=0") != std::string::npos,
          "intentional disabling neither queues nor counts disk loss");
    const auto reopen = Access::log_command(*restarted, {"reopen"});
    check(!reopen, "disabled reopen rejected");
    shell::SessionFeedback feedback;
    check(feedback.failure(reopen.error(), "log").find("save, and restart") != std::string::npos,
          "disabled reopen explains recovery");

    // Bypass the consumer to model a user-edited invalid file. Registration on
    // the next run must repair live values without rewriting saved values.
    take(Access::set_param(*restarted, "log_file_bytes", std::uint64_t{0}));
    take(Access::param_command(*restarted, {"save"}));
    restarted.reset();
    const auto parameter_file = directory.path / name / "params.toml";
    const auto before = read_file(parameter_file);
    auto repaired_start = take(Access::create(name, {}, test::host_path()));
    take(Access::configure_params(*repaired_start, directory.path));
    const auto repaired_options = take(Access::configure_log_from_params(*repaired_start, directory.path));
    check(!repaired_options.enabled && repaired_options.max_file_bytes == config::log_file_bytes &&
          repaired_options.max_total_bytes == config::log_total_bytes, "invalid startup pair uses defaults");
    check(read_file(parameter_file) == before, "repair does not save implicitly");
    repaired_start.reset();
    check(read_file(marker) == "untouched" && std::filesystem::last_write_time(marker) == marker_time,
          "disabled startup and shutdown preserve existing files");
}

void unavailable_and_no_files() {
    Directory directory;
    auto channels = take(control::Service::create());
    auto hub = take(output_hub::OutputHub::create());
    auto logs = take(log::detail::Manager::create(*channels, *hub, "disabled-test"));
    const auto absent = directory.path / "must-not-exist";
    take(logs->configure(absent, {.enabled = false}));
    logs->diagnostic("not a lost file record");
    check(!std::filesystem::exists(absent), "disabled configuration never creates directory");
    logs.reset();
    check(!std::filesystem::exists(absent), "disabled shutdown never creates directory");

    auto instance = take(detail::Instance::create(test::instance_name()));
    detail::LockRegistry locks(instance);
    const auto descriptor = take(locks.create());
    auto storage = take(param::detail::Storage::create(instance, descriptor.name, descriptor.id));
    auto params = take(param::detail::ParamManager::create(std::move(storage)));
    detail::LogSettings settings(*params);
    param::detail::ParamManagerTestAccess::close_admission(*params);
    auto selected = settings.read();
    check(!selected, "unavailable Param is not replaced by guessed defaults");
    auto unavailable = take(log::detail::Manager::create(*channels, *hub, "unavailable-test"));
    take(unavailable->configuration_unavailable(selected.error()));
    unavailable->diagnostic("configuration diagnosis remains visible");
    const auto status = take(unavailable->command({"status"}));
    check(status.find("file=unavailable") != std::string::npos &&
          status.find("file_lost=0") != std::string::npos, "unavailable distinct from disabled/loss");
    auto reopen = unavailable->command({"reopen"});
    check(!reopen && reopen.error().is(log::ErrorCode::ConfigurationUnavailable),
          "reopen cannot bypass missing settings");
}
}

int main() {
    try {
        settings_and_persistence();
        unavailable_and_no_files();
        std::cout << "Log settings tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
