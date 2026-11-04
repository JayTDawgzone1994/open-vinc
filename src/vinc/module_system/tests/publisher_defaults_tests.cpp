#include "../../param/tests/support.hpp"
#include <vinc/module/detail/publisher_defaults.hpp>
#include <vinc/module/detail/instance.hpp>
#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/param/detail/persistence.hpp>
#include <vinc/module/error.hpp>
#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <iostream>

namespace vinc::param::detail {
struct ParamManagerTestAccess {
    static Result<void> exhaust_versions(ParamManager& manager) {
        return manager.call([&manager]() -> Result<void> {
            manager.live_.revision = UINT64_MAX;
            return {};
        });
    }

    static void close_admission(ParamManager& manager) {
        std::lock_guard lock(manager.mutex_);
        manager.closing_ = true;
    }
};
}

namespace {
using namespace vinc;
using namespace param_test;
constexpr std::uint32_t changed_slots = 7;
constexpr std::uint64_t changed_bytes = 2048;
constexpr char slots_name[] = "vinc.pub_default_slots";
constexpr char bytes_name[] = "vinc.pub_default_message_bytes";

struct Fixture {
    Directory directory;
    std::shared_ptr<detail::Instance> instance = take(detail::Instance::create(unique_name()));
    detail::LockRegistry locks{instance};
    std::unique_ptr<param::detail::ParamManager> params;
    std::unique_ptr<detail::PublisherDefaults> defaults;

    explicit Fixture(message_bus::Limits limits = {}) {
        const auto lock = take(locks.create());
        auto storage = take(param::detail::Storage::create(instance, lock.name, lock.id));
        params = take(param::detail::ParamManager::create(std::move(storage)));
        defaults = std::make_unique<detail::PublisherDefaults>(*params, limits);
    }

    auto file() const {
        return directory.path / "params.toml";
    }

    void start() {
        take(params->configure(file()));
        take(defaults->initialize());
    }

    void set(std::uint32_t slots, std::uint64_t bytes) {
        take(params->set_framework("pub_default_slots", slots));
        take(params->set_framework("pub_default_message_bytes", bytes));
    }
};

void check_values(message_bus::detail::PublisherDefaults actual, std::uint32_t slots, std::uint64_t bytes) {
    check(actual.slots == slots && actual.max_message_bytes == bytes, "effective defaults differ");
}

void registration_and_persistence() {
    Fixture fixture;
    param::detail::SavedMap saved{
        {slots_name, {config::message_bus_default_slots, changed_slots}},
        {bytes_name, {config::message_bus_default_message_bytes, changed_bytes}}
    };
    {
        param::detail::Persistence writer;
        take(writer.load(fixture.file()));
        take(writer.write(saved));
    }
    fixture.start();
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    const auto values = take(fixture.params->read_current({slots_name, bytes_name}));
    for (const auto& [name, entry] : values) {
        check(entry.permission == param::Permission::Limited && entry.restart_hint == param::RestartHint::None,
              "framework defaults are Limited and do not require restart");
    }
    check(values.at(slots_name).default_value == param::Value(config::message_bus_default_slots),
          "saved override is not a new registered default");
    check(values.at(bytes_name).default_value == param::Value(config::message_bus_default_message_bytes),
          "saved byte override preserves the code default");

    auto independent = take(fixture.params->read_current({slots_name}));
    independent.at(slots_name).value = std::uint32_t{99};
    check(take(take(fixture.params->read_current({slots_name})).at(slots_name).get<std::uint32_t>()) == changed_slots,
          "framework reads return independent copies");
    const auto missing = fixture.params->read_current({"vinc.missing"});
    check(!missing && missing.error().is(param::ErrorCode::NotFound), "missing names remain errors");
    const auto foreign_write = fixture.params->set_framework("camera.gain", std::int32_t{1});
    check(!foreign_write && foreign_write.error().is(param::ErrorCode::InvalidName),
          "framework consumer writes use central short names only");

    fixture.set(changed_slots + 1, changed_bytes * 2);
    check_values(take(fixture.defaults->read()), changed_slots + 1, changed_bytes * 2);
    {
        param::detail::Persistence reader;
        const auto unchanged = take(reader.load(fixture.file()));
        check(unchanged.at(slots_name).value == param::Value(changed_slots), "live edits do not implicitly save");
    }
    take(fixture.params->command({"save"}));
    fixture.defaults.reset();
    fixture.params.reset();

    Fixture restarted;
    std::filesystem::copy_file(fixture.file(), restarted.file());
    restarted.start();
    check_values(take(restarted.defaults->read()), changed_slots + 1, changed_bytes * 2);
}

void invalid_values_restore_effective_pair() {
    Fixture fixture;
    fixture.start();
    fixture.set(changed_slots, changed_bytes);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    fixture.set(0, changed_bytes * 2);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    auto values = take(fixture.params->read_current({slots_name, bytes_name}));
    check(take(values.at(slots_name).get<std::uint32_t>()) == changed_slots &&
          take(values.at(bytes_name).get<std::uint64_t>()) == changed_bytes,
          "invalid candidate restores the last effective pair, not code defaults");
    std::uint64_t diagnostic_cursor = 0;
    check(fixture.params->diagnostic(diagnostic_cursor).contains("Param restored; not saved"),
          "rejection reports correction without claiming persistence");

    fixture.set(changed_slots, 0);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    fixture.set(changed_slots, UINT64_MAX);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    fixture.set(message_bus::detail::maximum_ring_buffer_slots + 1, changed_bytes);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    // Each field is individually in range; the pair exceeds an empty Bus budget.
    fixture.set(message_bus::detail::maximum_ring_buffer_slots, config::message_bus_default_message_bytes);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);

    fixture.set(1, changed_bytes);
    check_values(take(fixture.defaults->read()), 1, changed_bytes);
    fixture.set(0, changed_bytes);
    check_values(take(fixture.defaults->read()), 1, changed_bytes);
    check(take(take(fixture.params->read_current({slots_name})).at(slots_name).get<std::uint32_t>()) == 1,
          "one remains a valid requested count; Bus alone normalizes it to two");
}

void initial_invalid_and_failed_correction() {
    Fixture fixture;
    fixture.start();
    fixture.set(0, changed_bytes);
    check_values(take(fixture.defaults->read()), config::message_bus_default_slots,
                 config::message_bus_default_message_bytes);
    fixture.set(changed_slots, changed_bytes);
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    fixture.set(0, changed_bytes);
    // Exhaustion is an existing deterministic publication failure, not a new
    // production hook or a sleep-based race with the correction worker.
    take(param::detail::ParamManagerTestAccess::exhaust_versions(*fixture.params));
    check_values(take(fixture.defaults->read()), changed_slots, changed_bytes);
    check(take(take(fixture.params->read_current({slots_name})).at(slots_name).get<std::uint32_t>()) == 0,
          "failed correction does not pretend to have changed Param");
    std::uint64_t cursor = 0;
    check(fixture.params->diagnostic(cursor).contains("Param correction failed"),
          "failed correction retains a diagnostic while valid settings survive");
}

void failed_read_is_not_a_default() {
    Fixture fixture;
    fixture.start();
    take(fixture.defaults->read());
    param::detail::ParamManagerTestAccess::close_admission(*fixture.params);
    const auto failed = fixture.defaults->read();
    check(!failed && failed.error().is(module_system::ErrorCode::ParamFailed) &&
          failed.error().is_operation(module_system::ErrorOperation::AccessParam),
          "read failure propagates instead of silently using code defaults");
}

void completed_registration_can_be_confirmed() {
    Fixture fixture;
    take(fixture.params->register_framework("pub_default_slots", config::message_bus_default_slots));
    take(fixture.params->set_framework("pub_default_slots", changed_slots));
    // Same state as a committed registration whose caller did not observe
    // completion. Confirmation must preserve live edits and register the rest.
    take(fixture.defaults->initialize());
    check_values(take(fixture.defaults->read()), changed_slots, config::message_bus_default_message_bytes);
    take(fixture.defaults->initialize());

    Fixture collision;
    take(collision.params->register_framework("pub_default_slots", changed_slots));
    const auto rejected = collision.defaults->initialize();
    check(!rejected && rejected.error().is(module_system::ErrorCode::ParamFailed),
          "a conflicting central declaration is not silently accepted or overwritten");
}
}

int main() {
    try {
        registration_and_persistence();
        invalid_values_restore_effective_pair();
        initial_invalid_and_failed_correction();
        failed_read_is_not_a_default();
        completed_registration_can_be_confirmed();
        std::cout << "PASS: Publisher Param defaults, copies, restoration, persistence and failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
