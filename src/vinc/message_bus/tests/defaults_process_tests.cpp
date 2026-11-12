#include "../../../../tests/support/manager.hpp"
#include "../../param/tests/support.hpp"
#include <vinc/param/detail/persistence.hpp>
#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <iostream>

namespace {
using namespace vinc;
using namespace param_test;
using namespace std::chrono_literals;
constexpr auto startup_timeout = 15s;
constexpr auto command_timeout = 5s;
constexpr auto shutdown_timeout = 10s;
constexpr std::uint32_t changed_slots = 3;
constexpr std::uint64_t changed_bytes = 2048;
constexpr char slots_name[] = "vinc.pub_default_slots";
constexpr char bytes_name[] = "vinc.pub_default_message_bytes";

void run(const std::filesystem::path& plugin) {
    Directory directory;
    const auto identity = test::instance_name();
    const auto create = [&] {
        auto manager = take(detail::ManagerAccess::create(identity, {}, test::host_path()));
        take(detail::ManagerAccess::configure_params(*manager, directory.path));
        ModulePolicy policy;
        policy.startup_timeout = startup_timeout;
        policy.max_restarts = 0;
        take(manager->register_module_type({"messageBusTest", plugin, policy}));
        return manager;
    };
    auto manager = create();
    const auto start = [&](std::string name, std::vector<std::string> options = {}) {
        std::vector<std::string> args{"--name", name, "pub", "defaults/" + name};
        args.insert(args.end(), options.begin(), options.end());
        const auto mid = take(manager->start_module("messageBusTest", std::move(args)));
        take(manager->wait_running(mid, control::Clock::now() + startup_timeout));
        return mid;
    };
    const auto command = [&](MID mid, const char* operation) {
        return take(manager->command(mid, {operation}, control::Clock::now() + command_timeout));
    };
    const auto set = [&](const char* name, std::string value) {
        take(detail::ManagerAccess::param_command(*manager, {"set", name, std::move(value)}));
    };
    const auto shutdown = [&] {
        take(manager->wait_shutdown(control::Clock::now() + shutdown_timeout));
        manager.reset();
    };

    const auto initial = take(detail::ManagerAccess::param_snapshot(*manager));
    check(initial.entries.contains(slots_name) && initial.entries.contains(bytes_name),
          "Publisher defaults exist before the first Module");
    auto original = start("original");
    check(command(original, "slots") == config::message_bus_default_slots &&
          command(original, "bytes") == config::message_bus_default_message_bytes,
          "first Module gets code defaults through central resolution");
    check(command(original, "write-default") == 0, "ordinary Modules cannot modify Limited central parameters");

    set(slots_name, std::to_string(changed_slots));
    set(bytes_name, std::to_string(changed_bytes));
    const auto changed = start("changed");
    check(command(changed, "slots") == changed_slots && command(changed, "bytes") == changed_bytes,
          "later Module uses current committed defaults");
    check(command(original, "slots") == config::message_bus_default_slots &&
          command(original, "bytes") == config::message_bus_default_message_bytes,
          "an already mapped ring never changes");
    const auto partial = start("partial", {"5"});
    check(command(partial, "slots") == 5 && command(partial, "bytes") == changed_bytes,
          "partial options cross the process boundary without baking in the other default");

    set(slots_name, "0");
    const auto explicit_options = start("explicit", {"1", "4096"});
    check(command(explicit_options, "requested-slots") == 1 && command(explicit_options, "slots") == 2,
          "explicit capacity ignores invalid central defaults and keeps one-to-two normalization");
    check(command(explicit_options, "default-slots") == 0,
          "fully explicit registration does not read or correct unused defaults");
    const auto corrected = start("corrected");
    check(command(corrected, "slots") == changed_slots && command(corrected, "bytes") == changed_bytes,
          "invalid default retains the last effective pair");
    check(command(corrected, "default-slots") == changed_slots, "central correction is visible to Module reads");
    std::uint64_t cursor = 0;
    check(detail::ManagerAccess::param_diagnostic(*manager, cursor).contains("Publisher defaults rejected"),
          "central reports the rejected values");

    take(detail::ManagerAccess::param_command(*manager, {"save"}));
    set(slots_name, "9");
    set(bytes_name, "8192");
    const auto unsaved = start("unsaved");
    check(command(unsaved, "slots") == 9 && command(unsaved, "bytes") == 8192, "live defaults can change again");
    shutdown();

    param::detail::Persistence saved;
    const auto file = directory.path / identity / "params.toml";
    const auto entries = take(saved.load(file));
    check(entries.at(slots_name).value == param::Value(changed_slots) &&
          entries.at(bytes_name).value == param::Value(changed_bytes), "shutdown does not save live changes");
    manager = create();
    const auto restarted = start("restarted");
    check(command(restarted, "slots") == changed_slots && command(restarted, "bytes") == changed_bytes,
          "a new central process loads saved defaults before starting its Modules");
    shutdown();
}

void exhausted_budget_keeps_defaults(const std::filesystem::path& plugin) {
    Directory directory;
    constexpr std::uint32_t slots = 2;
    // The filled nested Frame is 1040 bytes on the validated MSVC build.
    // This case exhausts the number of rings, not the size of one valid image.
    constexpr std::uint64_t bytes = 4096;
    message_bus::Limits limits;
    limits.subscribers_per_topic = 1;
    limits.max_shared_bytes = message_bus::detail::RingBuffer::control_bytes(slots, limits.subscribers_per_topic) +
        slots * message_bus::detail::RingBuffer::slot_stride(bytes) + sizeof(message_bus::detail::SharedWord);
    auto manager = take(test::create_manager(limits));
    take(detail::ManagerAccess::configure_params(*manager, directory.path));
    take(detail::ManagerAccess::param_command(*manager, {"set", slots_name, std::to_string(slots)}));
    take(detail::ManagerAccess::param_command(*manager, {"set", bytes_name, std::to_string(bytes)}));
    ModulePolicy policy;
    policy.startup_timeout = startup_timeout;
    policy.max_restarts = 0;
    take(manager->register_module_type({"messageBusTest", plugin, policy}));
    const auto first = take(manager->start_module("messageBusTest", {"--name", "first", "pub", "budget/topic"}));
    take(manager->wait_running(first, control::Clock::now() + startup_timeout));
    const auto compact = take(manager->command(first, {"compact-bytes"}, control::Clock::now() + command_timeout));
    check(compact > 0 && static_cast<std::uint64_t>(compact) <= bytes,
          "budget fixture must fit the representative message before testing ring exhaustion");
    check(take(manager->command(first, {"push", "41"}, control::Clock::now() + command_timeout)) == 0,
          "first Publisher must work before exhausting the ring budget");
    const auto second = take(manager->start_module("messageBusTest", {"--name", "second", "pub", "budget/topic"}));
    const auto rejected = manager->wait_running(second, control::Clock::now() + startup_timeout);
    check(!rejected && rejected.error().is(module_system::ErrorCode::HookFailed),
          "exhausted budget rejects only the second Module startup");
    const auto causes = rejected.error().causes();
    check(!causes.empty() && causes.back().domain == ErrorDomain::MessageBus &&
          causes.back().code == static_cast<std::int32_t>(message_bus::ErrorCode::ResourceExhausted),
          "real memory-budget exhaustion remains the deepest cause");
    const auto values = take(detail::ManagerAccess::param_snapshot(*manager));
    check(take(values.entries.at(slots_name).get<std::uint32_t>()) == slots &&
          take(values.entries.at(bytes_name).get<std::uint64_t>()) == bytes,
          "current allocation pressure never rewrites valid defaults");
    const auto pushed = take(manager->command(first, {"push", "42"}, control::Clock::now() + command_timeout));
    if (pushed != 0) {
        throw std::runtime_error("existing Publisher push failed: helper code=" + std::to_string(pushed) +
            ", compact bytes=" + std::to_string(compact) + ", budget=" + std::to_string(bytes));
    }
    take(manager->wait_shutdown(control::Clock::now() + shutdown_timeout));
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected Module Plugin");
        run(std::filesystem::absolute(argv[1]));
        exhausted_budget_keeps_defaults(std::filesystem::absolute(argv[1]));
        std::cout << "PASS: Publisher Param defaults across real Host/Plugin processes and central restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
