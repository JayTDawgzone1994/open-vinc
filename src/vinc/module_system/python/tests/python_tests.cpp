#include "../../../../../tests/support/manager.hpp"
#include <vinc/module/host/python_modules.hpp>
#include <vinc/module/detail/python_launch.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/osal/path.hpp>
#include <fstream>
#include <array>
#include <random>
#include <iostream>
#include <thread>
#include <stdexcept>

using namespace vinc;
using Clock = control::Clock;
using namespace std::chrono_literals;
namespace {
constexpr auto test_timeout = 8s;
constexpr auto command_timeout = 2s;
constexpr auto observation_interval = 10ms;
constexpr auto stop_grace = 500ms;
constexpr auto startup_timeout = 1500ms;
constexpr auto heartbeat_interval = 100ms;
constexpr auto heartbeat_timeout = 700ms;
constexpr auto retry_delay = 150ms;
constexpr auto blocked_command_timeout = 100ms;
constexpr auto orphan_heartbeat_interval = 40ms;
constexpr auto orphan_heartbeat_timeout = 400ms;
constexpr auto orphan_startup_timeout = 2s;
constexpr auto orphan_stop_timeout = 200ms;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

ModuleSnapshot finished(ModuleManager& manager, MID mid) {
    const auto deadline = Clock::now() + test_timeout;
    while (Clock::now() < deadline) {
        auto record = manager.inspect(mid);
        if (record && (record->state == ModuleState::Stopped || record->state == ModuleState::Failed)) {
            return *record;
        }

        std::this_thread::sleep_for(observation_interval);
    }

    throw std::runtime_error("Python process did not finish in time");
}

constexpr auto raw_exit_timeout = 5s;
constexpr auto raw_frame_timeout = 3s;
constexpr auto raw_name_reply_timeout = 1s;
constexpr auto cleanup_timeout = 2s;
constexpr std::size_t raw_receive_buffer_bytes = 1024;
constexpr int raw_listener_backlog = 4;

Result<osal::Socket> make_raw_listener(const std::string& endpoint) {
    auto listener = take(osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream));
    take(listener.bind(osal::LocalEndpoint{endpoint}));
    take(listener.listen(raw_listener_backlog));
    return listener;
}

void test_parent_loss(const std::filesystem::path& python, const std::filesystem::path& bootstrap,
                      const std::filesystem::path& helper) {
    // Own the real module child directly to verify self-exit when its central
    // stops heartbeats without closing the socket, and when the socket closes.
    auto process_service = osal::ProcessService::create();
    check(bool(process_service), "orphan test process service");

    struct EndpointCleanup {
        std::filesystem::path path;

        ~EndpointCleanup() {
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
        }
    } endpoint{std::filesystem::temp_directory_path() /
               ("ov-hb-" + std::to_string(std::random_device{}()) + ".sock")};

    const auto endpoint_u8 = endpoint.path.u8string();
    // Local-socket path boundary: char inspects UTF-8 code units without transcoding.
    const std::string endpoint_text(reinterpret_cast<const char*>(endpoint_u8.data()), endpoint_u8.size());
    auto listener = make_raw_listener(endpoint_text);

    for (bool disconnect : {false, true}) {
        auto report = take(detail::ContractReportStorage::create(
            "orphan_" + std::to_string(std::random_device{}()), 1234));
        osal::SpawnOptions spawn;

        spawn.arguments = {detail::bootstrap::version_marker,
                           endpoint_text,
                           "orphan-test",
                           "1234",
                           std::to_string(orphan_heartbeat_interval.count()),
                           std::to_string(orphan_heartbeat_timeout.count()),
                           std::to_string(std::chrono::milliseconds(orphan_startup_timeout).count()),
                           std::to_string(orphan_stop_timeout.count()),
                           "helper",
                           report->name(), take(osal::path_to_utf8(helper)), "0", "--name", "orphan"};
        take(detail::prepare_python_spawn(spawn, PythonLaunch{helper, bootstrap, python}));
        auto child = process_service->spawn(spawn);
        check(bool(child), "spawn orphan test");

        struct ChildCleanup {
            osal::Process& child;
            osal::ProcessService& service;

            ~ChildCleanup() {
                (void)child.terminate();
                (void)child.wait_until(control::Clock::now() + cleanup_timeout);
                (void)service.forget(child.id());
            }
        } cleanup{*child, *process_service};

        auto socket = listener->accept_until(control::Clock::now() + raw_exit_timeout);
        check(bool(socket), "accept orphan");
        control::Frame hello{control::Kind::Hello};
        control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Central));
        control::put_u64(hello.payload, 1234);
        check(bool(control::put_string(hello.payload, "orphan-test")), "encode token");
        control::put_u32(hello.payload, static_cast<std::uint32_t>(orphan_heartbeat_interval.count()));
        control::put_u32(hello.payload, static_cast<std::uint32_t>(orphan_heartbeat_timeout.count()));
        control::put_u32(hello.payload, control::hello_version);
        control::put_u32(hello.payload, 1u);
        auto bytes = take(control::encode(hello));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto sent = socket->send_until(std::span<const std::byte>(bytes).subspan(offset),
                                           control::Clock::now() + raw_frame_timeout);
            check(sent && *sent, "send central handshake");
            offset += *sent;
        }
        // Wait for Ready so the test distinguishes watchdog exit from failed startup.
        control::Decoder decoder;
        bool ready = false;
        std::array<std::byte, raw_receive_buffer_bytes> buffer;
        while (!ready) {
            auto received = socket->receive_until(buffer, control::Clock::now() + raw_frame_timeout);
            check(received && !received->eof, "receive module ready");
            auto frames = decoder.feed(std::span<const std::byte>(buffer).first(received->bytes));
            check(bool(frames), "decode ready");
            for (const auto& frame : *frames) {
                if (frame.kind == control::Kind::Request && frame.domain == 1 && frame.operation == 3) {
                    control::Frame approved{control::Kind::Response, 1, 3, frame.request};
                    control::put_u32(approved.payload, 0);
                    check(bool(control::put_string(approved.payload, "orphan")), "encode name");
                    const auto encoded = take(control::encode(approved));
                    std::size_t sent = 0;
                    while (sent < encoded.size()) {
                        auto bytes_sent =
                            socket->send_until(std::span<const std::byte>(encoded).subspan(sent),
                                               control::Clock::now() + raw_name_reply_timeout);
                        check(bytes_sent && *bytes_sent, "approve orphan name");
                        sent += *bytes_sent;
                    }
                }

                if (frame.kind == control::Kind::Event && frame.domain == 1 && frame.operation == 1) {
                    ready = true;
                }
            }
        }

        if (disconnect) {
            check(bool(socket->close()), "close central socket");
        }

        auto exit = child->wait_until(control::Clock::now() + raw_exit_timeout);
        check(exit && exit->value == 72, disconnect ? "module exits when central disconnects"
                                                    : "module exits when central heartbeat stops");
    }
}

void lifecycle(const std::filesystem::path& python, const std::filesystem::path& bootstrap,
               const std::filesystem::path& fixtures) {
    auto manager = take(test::create_manager());
    auto discovery = take(discover_python_modules(*manager, fixtures, bootstrap, python));
    check(discovery.registered == 4 && discovery.issues.empty(), "discovery must not execute import errors/hangs");
    auto duplicate = take(discover_python_modules(*manager, fixtures, bootstrap, python));
    check(duplicate.registered == 0 && duplicate.issues.size() == 4, "duplicate types are rejected");
    const auto mid = take(manager->start_module("framework", {"framework", "--name", "front"}));
    take(manager->wait_started(mid));
    check(take(manager->command(mid, {"front", "status"}, Clock::now() + command_timeout)) == 42,
          "shell hook strips instance name");
    check(take(manager->command(mid, {"front", "caught"}, Clock::now() + command_timeout)) == 17,
          "Result becomes inspectable/catchable Python exception");
    check(take(manager->command(mid, {"bad-command"}, Clock::now() + command_timeout)) == 1,
          "nonzero command status is not a module failure");
    check(take(manager->complete(mid, {"front", ""}, Clock::now() + command_timeout)).size() == 4,
          "completion hook");
    std::this_thread::sleep_for(heartbeat_timeout * 2);
    check(manager->inspect(mid)->state == ModuleState::Running, "idle heartbeat remains native");
    auto collision = take(manager->start_module("framework", {"--name", "front"}));
    check(!manager->wait_started(collision), "duplicate instance name refused");
    auto reserved = take(manager->start_module("framework", {"--name", "framework"}));
    check(!manager->wait_started(reserved), "type cannot be an instance name");
    auto missing_name = take(manager->start_module("framework"));
    check(!manager->wait_started(missing_name), "name is required");
    auto crashed = manager->command(mid, {"front", "crash"}, Clock::now() + command_timeout);
    check(!crashed, "uncaught shell exception reports command failure");
    auto final = finished(*manager, mid);
    check(final.state == ModuleState::Failed && final.last_failure, "uncaught shell exception fails module");
    check(final.last_failure->error.is_operation(module_system::ErrorOperation::CommandHook),
          "retain escaped hook operation");
    auto escaped = take(manager->start_module("framework", {"--name", "escapedNode"}));
    take(manager->wait_started(escaped));
    (void)manager->command(escaped, {"escape-vinc"}, Clock::now() + command_timeout);
    const auto escaped_record = finished(*manager, escaped);
    check(escaped_record.last_failure.has_value(), "retain uncaught VINC failure");
    const auto causes = escaped_record.last_failure->error.causes();
    check(!causes.empty() && causes.back().domain == ErrorDomain::ModuleSystem &&
          causes.back().code == static_cast<std::int32_t>(module_system::ErrorCode::Busy),
          "Python exception retains root cause through central protocol");
    for (const auto& mode : {"finish", "main-error", "thread"}) {
        auto manual = take(manager->start_module("manual", {"--name", "manualNode", mode}));
        if (std::string_view(mode) == "thread") {
            take(manager->wait_started(manual));
            check(take(manager->command(manual, {}, Clock::now() + command_timeout)) == 24, "manual pump thread");
            take(manager->stop_module(manual, Clock::now() + test_timeout));
        }

        const auto record = finished(*manager, manual);
        check(record.state == (std::string_view(mode) == "main-error" ? ModuleState::Failed : ModuleState::Stopped),
              "manual completion/failure policy");
    }

    for (const auto& mode : {"start-error", "stop-error"}) {
        auto failed = take(manager->start_module("framework", {"--name", "failureNode", mode}));
        if (std::string_view(mode) == "start-error") {
            check(!manager->wait_started(failed), "start exception");
        } else {
            take(manager->wait_started(failed));
            (void)manager->stop_module(failed, Clock::now() + test_timeout);
        }

        const auto record = finished(*manager, failed);
        check(record.last_failure || record.cleanup_failure, "preserve start/stop diagnostic");
    }

    auto imported = take(manager->start_module("loaderror", {"--name", "badImport"}));
    check(!manager->wait_started(imported), "import error isolated from central");
#if defined(_WIN32) && defined(_MSC_VER)
    {
        // Exercise a failure before _native is loaded. The temporary SDK has
        // no native binary, so successful rejection cannot depend on its code.
        struct TemporarySdk {
            std::filesystem::path path;

            ~TemporarySdk() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } sdk{std::filesystem::temp_directory_path() /
              ("vinc-runtime-test-" + std::to_string(std::random_device{}()))};
        const auto package = sdk.path / "vinc";
        std::filesystem::create_directories(package);
        for (const auto* filename : {"__init__.py", "_build.py", "_windows_runtime.py", "bootstrap.py"}) {
            std::filesystem::copy_file(bootstrap.parent_path() / filename, package / filename);
        }

        {
            std::ofstream metadata(package / "_msvc_runtime.json");
            metadata << R"({"format":99})";
            check(bool(metadata), "write invalid Runtime metadata fixture");
        }

        ModuleType incompatible;
        incompatible.name = "runtimeMismatch";
        incompatible.python = PythonLaunch{fixtures / "framework/framework.py", package / "bootstrap.py", python};
        take(manager->register_module_type(incompatible));
        const auto survivor = take(manager->start_module("framework", {"--name", "runtimeSurvivor"}));
        take(manager->wait_started(survivor));
        auto rejected = take(manager->start_module(incompatible.name, {"--name", "rejectedRuntime"}));
        check(!manager->wait_started(rejected), "Runtime preflight failure rejects module startup");
        const auto record = finished(*manager, rejected);
        check(record.state == ModuleState::Failed && record.last_exit && record.last_exit->value == 70,
              "Runtime preflight exits through Python bootstrap, not access violation");

        check(take(manager->command(survivor, {"status"}, Clock::now() + command_timeout)) == 42,
              "central and existing module survive Runtime preflight failure");
        take(manager->stop_module(survivor, Clock::now() + test_timeout));
    }
#endif
    ModuleType retry;
    retry.name = "retryType";

    retry.policy.max_restarts = 1;
    retry.policy.restart_delay = retry_delay;
    retry.python = PythonLaunch{fixtures / "manual/manual.py", bootstrap, python};
    take(manager->register_module_type(retry));
    const auto retry_mid = take(manager->start_module(retry.name, {"--name", "retryNode", "main-error"}));
    const auto retry_deadline = Clock::now() + test_timeout;
    bool retried = false;
    while (Clock::now() < retry_deadline) {
        const auto record = manager->inspect("retryNode");
        if (record && record->state == ModuleState::Failed) {
            check(record->restarts == 1 && record->mid != retry_mid, "restart allocates a fresh MID");
            retried = true;
            break;
        }

        std::this_thread::sleep_for(observation_interval);
    }

    check(retried, "Python fault honors retry budget");
    // Native stop supervision must work even when a hook monopolizes Python execution.
    ModuleType blocking;
    blocking.name = "blockingType";

    blocking.policy.stop_timeout = stop_grace;
    blocking.python = PythonLaunch{fixtures / "framework/framework.py", bootstrap, python};
    take(manager->register_module_type(blocking));
    auto blocked = take(manager->start_module(blocking.name, {"--name", "blockingNode"}));
    take(manager->wait_started(blocked));
    (void)manager->command(blocked, {"block"}, Clock::now() + blocked_command_timeout);
    (void)manager->stop_module(blocked, Clock::now() + test_timeout);
    check(finished(*manager, blocked).last_exit.has_value(), "blocked hook terminated");
    blocking.name = "importHang";
    blocking.policy.startup_timeout = startup_timeout;
    blocking.python->script = fixtures / "loadhang/loadhang.py";
    take(manager->register_module_type(blocking));
    auto hang = take(manager->start_module(blocking.name, {"--name", "importHangNode"}));
    check(!manager->wait_started(hang), "hung import bounded by startup deadline");
    check(finished(*manager, hang).last_exit.has_value(), "hung import reaped");
    blocking.name = "missingPython";
    blocking.python->executable = fixtures / "does-not-exist-python";
    take(manager->register_module_type(blocking));
    auto missing = take(manager->start_module(blocking.name));
    check(!manager->wait_started(missing), "missing interpreter does not kill central");
    manager->begin_shutdown();
    take(manager->wait_shutdown(Clock::now() + test_timeout));
}
}

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }

    try {
        lifecycle(std::filesystem::absolute(argv[1]), std::filesystem::absolute(argv[2]),
                  std::filesystem::absolute(argv[3]));
        test_parent_loss(std::filesystem::absolute(argv[1]), std::filesystem::absolute(argv[2]),
                         std::filesystem::absolute(argv[3]) / "framework/framework.py");
        std::cout << "Python lifecycle passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
