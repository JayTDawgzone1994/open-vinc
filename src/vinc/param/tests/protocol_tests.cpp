#include "support.hpp"
#include "../../control_channel/tests/channel_test_support.hpp"
#include <vinc/module/detail/module_params.hpp>
#include <vinc/module/detail/instance.hpp>
#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/param/detail/manager.hpp>
#include <vinc/param/detail/client_access.hpp>
#include <vinc/module/base_module.hpp>
#include <future>
#include <iostream>

namespace vinc::param::detail {
struct ParamManagerTestAccess {
    static Result<void> enqueue(ParamManager& manager, std::function<void()> job) {
        return manager.enqueue(std::move(job));
    }
};
}

namespace {
using namespace vinc;
using namespace param_test;
namespace wire = vinc::param::detail;
constexpr MID test_mid = 7;
constexpr std::uint64_t unrelated_lock = 99;

void queue_capacity() {
    auto instance = take(detail::Instance::create(unique_name()));
    detail::LockRegistry locks(instance);
    auto lock = take(locks.create());
    auto storage = take(wire::Storage::create(instance, lock.name, lock.id));
    auto manager = take(wire::ParamManager::create(std::move(storage)));
    take(manager->register_framework("gain", std::int32_t{1}));

    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    // Release the worker on every assertion/unwind path, before Manager joins it.
    struct ReleaseWorker {
        std::promise<void>& signal;
        ~ReleaseWorker() {
            signal.set_value();
        }
    };
    std::vector<std::size_t> executed;
    executed.reserve(config::param_queue_limit);
    std::promise<void> drained;
    auto finished = drained.get_future();
    struct JoinWorker {
        std::unique_ptr<wire::ParamManager>& manager;
        ~JoinWorker() {
            manager.reset();
        }
    } join{manager};
    {
        ReleaseWorker cleanup{release};
        take(wire::ParamManagerTestAccess::enqueue(*manager, [&] {
            entered.set_value();
            released.wait();
        }));
        check(started.wait_until(deadline()) == std::future_status::ready, "queue gate entered");
        for (std::size_t index = 0; index < config::param_queue_limit; ++index) {
            take(wire::ParamManagerTestAccess::enqueue(*manager, [&, index] {
                executed.push_back(index);
                if (index + 1 == config::param_queue_limit) {
                    drained.set_value();
                }
            }));
        }
        const auto rejected = manager->command({"set", "vinc.gain", "9"});
        check(!rejected && rejected.error().is(param::ErrorCode::CapacityExceeded),
              "full queue rejects mutation before execution");
        const auto unsaved = manager->command({"save"});
        check(!unsaved && unsaved.error().is(param::ErrorCode::CapacityExceeded),
              "full queue rejects save rather than claiming success");
    }
    check(finished.wait_until(deadline()) == std::future_status::ready, "accepted queue drains");
    check(executed.size() == config::param_queue_limit, "no accepted jobs disappear");
    for (std::size_t index = 0; index < executed.size(); ++index) {
        check(executed[index] == index, "worker preserves admission order");
    }
    check(take(take(manager->inspect()).entries.at("vinc.gain").get<std::int32_t>()) == 1,
          "rejected mutation never appears later");
    take(manager->command({"set", "vinc.gain", "2"}));
    check(take(take(manager->inspect()).entries.at("vinc.gain").get<std::int32_t>()) == 2,
          "queue accepts new work after drain");
}

class ReadModule final : public BaseModule<RunMode::FrameworkLoop> {
public:
    param::Params& params() {
        return context.param;
    }
    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};

class SnapshotAccess final : public wire::Access {
public:
    std::optional<param::Snapshot> image = param::Snapshot{};
    std::optional<Error> failure;
    std::size_t reads = 0;
    std::size_t registrations = 0;
    param::RegisterOptions last_options;
    std::string_view module_name() const noexcept override {
        return "camera";
    }
    Result<void> register_value(std::string, param::Value, param::RegisterOptions options) override {
        ++registrations;
        last_options = options;
        return {};
    }
    Result<void> set_value(std::string, param::Value) override {
        return {};
    }
    Result<void> watch_value(std::string, param::Type) override {
        return {};
    }
    Result<std::optional<param::Snapshot>> snapshot(std::uint64_t known) override {
        ++reads;
        check(known == UINT64_MAX, "current read requests an unconditional snapshot");
        if (failure) {
            return std::unexpected(*failure);
        }
        return image;
    }
};

void current_read_errors() {
    ReadModule module;
    auto& params = module.params();
    const auto unbound = params.read_current("gain");
    check(!unbound && unbound.error().is(param::ErrorCode::Closed) &&
          unbound.error().is_operation(param::ErrorOperation::ReadCurrent), "unbound current read");
    SnapshotAccess access;
    wire::ClientAccess::bind(params, access);
    const auto invalid = params.read_current("camera/gain");
    check(!invalid && invalid.error().is(param::ErrorCode::InvalidName) && access.reads == 0,
          "invalid name is rejected before Host access");
    const auto missing = params.read_current("gain");
    check(!missing && missing.error().is(param::ErrorCode::NotFound), "empty image means missing value");
    access.image.reset();
    const auto absent_image = params.read_current("gain");
    check(!absent_image && absent_image.error().is(param::ErrorCode::Protocol),
          "unchanged image sentinel cannot satisfy unconditional current read");
    const auto native = Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory};
    access.failure = wrap_error(param::ErrorCode::StorageFailed, param::ErrorOperation::Attach, native);
    const auto failed = params.read_current("gain");
    check(!failed && failed.error().is(param::ErrorCode::StorageFailed) &&
          failed.error().is_operation(param::ErrorOperation::ReadCurrent) &&
          failed.error().causes().size() == 2 &&
          failed.error().causes().back().domain == ErrorDomain::Osal, "current read retains Host/native causes");
    check(error_to_string(failed.error()).find("ReadCurrent") != std::string::npos,
          "current read operation is in static error catalog");
    check(params.watches().empty(), "failed current reads never register watches");
}

void registration_options() {
    ReadModule module;
    SnapshotAccess access;
    auto& params = module.params();
    wire::ClientAccess::bind(params, access);
    take(params.register_param("plain", std::int32_t{1}));
    check(access.last_options.permission == param::Permission::Limited &&
          access.last_options.restart_hint == param::RestartHint::None, "omitted options defaults");
    take(params.register_param("restart", std::int32_t{1}, {.restart_hint = param::RestartHint::Module}));
    check(access.last_options.permission == param::Permission::Limited &&
          access.last_options.restart_hint == param::RestartHint::Module, "restart-only options");
    take(params.register_param("shared", std::int32_t{1}, {.permission = param::Permission::Public}));
    check(access.last_options.permission == param::Permission::Public &&
          access.last_options.restart_hint == param::RestartHint::None, "permission-only options");
    const auto invalid = params.register_param("invalid", std::int32_t{1},
        {.restart_hint = static_cast<param::RestartHint>(UINT32_MAX)});
    check(!invalid && invalid.error().is(param::ErrorCode::InvalidValue) && access.registrations == 3 &&
          params.watches().size() == 3, "invalid options rejected before watch or Host mutation");
}

struct Sessions {
    std::unique_ptr<control::Service> central_service = take(control::Service::create(1));
    std::unique_ptr<control::Service> module_service = take(control::Service::create(1));
    std::shared_ptr<control::Session> central;
    std::shared_ptr<control::Session> module;

    Sessions() {
        auto sockets = channel_test::connected_sockets();
        auto options = channel_test::options();
        central = take(central_service->attach(std::move(sockets.first), options));
        options.role = control::Role::Module;
        module = take(module_service->attach(std::move(sockets.second), options));
        take(central->wait_open(deadline()));
        take(module->wait_open(deadline()));
    }
};

control::Bytes descriptor(std::uint32_t version, std::uint64_t bytes = wire::storage_bytes) {
    control::Bytes result;
    control::put_u32(result, wire::result_success);
    control::put_u32(result, version);
    control::put_u64(result, unrelated_lock);
    control::put_u64(result, bytes);
    take(control::put_string(result, "absent-param-test"));
    return result;
}

void host_protocol() {
    Sessions sessions;
    detail::ModuleLocks locks(sessions.module, test_mid);
    detail::ModuleParams params(sessions.module, locks);
    params.identify("camera");
    const auto exchange = [&](control::Bytes body) {
        auto pending = std::async(std::launch::async, [&] {
            return params.snapshot(UINT64_MAX);
        });
        const auto request = take(sessions.central->receive_until(deadline()));
        check(request.domain == wire::domain && request.operation == wire::Attach, "Host attach request");
        take(sessions.central->send(
            {control::Kind::Response, wire::domain, wire::Attach, request.request, std::move(body)}));
        take(params.receive(take(sessions.module->receive_until(deadline()))));
        return pending.get();
    };
    for (auto bytes : {descriptor(wire::version - 1), descriptor(wire::version + 1),
                       descriptor(wire::version, wire::storage_bytes + 1)}) {
        const auto rejected = exchange(std::move(bytes));
        check(!rejected && rejected.error().is(param::ErrorCode::Protocol),
              "incompatible snapshot descriptor");
    }
    auto truncated = descriptor(wire::version);
    truncated.pop_back();
    check(!exchange(std::move(truncated)), "truncated descriptor");
    auto trailing = descriptor(wire::version);
    trailing.push_back(std::byte{0});
    check(!exchange(std::move(trailing)), "trailing descriptor");
    const auto cause = Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory};
    const auto error = wrap_error(param::ErrorCode::StorageFailed, param::ErrorOperation::Attach, cause);
    const auto failure = exchange(wire::encode_failure(error));
    check(!failure && failure.error().causes().size() == 1 &&
              failure.error().causes().front().domain == ErrorDomain::Osal,
          "Host retains remote causes");
    auto unknown = params.receive({control::Kind::Response, wire::domain, wire::Set, UINT64_MAX, {}});
    check(!unknown && unknown.error().is(param::ErrorCode::Protocol), "unknown response ID");

    // Cancellation only cancels waiting; the already sent mutation may still commit.
    auto pending = std::async(std::launch::async, [&] {
        return params.set_value("camera.gain", std::int32_t{9});
    });
    const auto request = take(sessions.central->receive_until(deadline()));
    params.stop();
    check(pending.wait_for(test_timeout) == std::future_status::ready, "pending mutation cancelled");
    const auto cancelled = pending.get();
    check(!cancelled && cancelled.error().is(param::ErrorCode::CancelledOutcomeUnknown) &&
              cancelled.error().is_operation(param::ErrorOperation::Set),
          "unknown mutation outcome");
    take(params.receive({control::Kind::Response, wire::domain, wire::Set, request.request, {}}));
    check(!params.watch_value("camera.gain", param::Type::Int32), "closed Host rejects new work");
}

void current_read_cancellation() {
    Sessions sessions;
    detail::ModuleLocks locks(sessions.module, test_mid);
    detail::ModuleParams host(sessions.module, locks);
    host.identify("camera");
    ReadModule module;
    wire::ClientAccess::bind(module.params(), host);
    auto active = std::async(std::launch::async, [&] {
        return module.params().read_current("gain");
    });
    const auto request = take(sessions.central->receive_until(deadline()));
    check(request.operation == wire::Attach, "first read attaches through Host");
    // Keep the active attach pending while another reader competes for the
    // Host buffer. Both must stop without a reply or an application pump.
    auto queued = std::async(std::launch::async, [&] {
        return module.params().read_current("gain");
    });
    host.stop();
    for (auto* future : {&active, &queued}) {
        check(future->wait_for(test_timeout) == std::future_status::ready, "all current readers cancel");
        const auto result = future->get();
        check(!result && result.error().is(param::ErrorCode::Cancelled) &&
              result.error().is_operation(param::ErrorOperation::ReadCurrent), "read cancellation is recoverable");
    }
    const auto stopped = module.params().read_current("gain");
    check(!stopped && stopped.error().is(param::ErrorCode::Cancelled), "stopped Host rejects fresh reads");
}

void current_read_timeout() {
    Sessions sessions;
    detail::ModuleLocks locks(sessions.module, test_mid);
    detail::ModuleParams host(sessions.module, locks);
    host.identify("camera");
    ReadModule module;
    wire::ClientAccess::bind(module.params(), host);
    auto pending = std::async(std::launch::async, [&] {
        return module.params().read_current("gain");
    });
    const auto request = take(sessions.central->receive_until(deadline()));
    check(request.operation == wire::Attach, "timeout test reached read-only attachment");
    const bool finished = pending.wait_for(config::param_request_timeout + test_timeout) ==
                          std::future_status::ready;
    host.stop();
    check(finished, "unanswered current read has a bounded wait");
    const auto result = pending.get();
    check(!result && result.error().is(param::ErrorCode::Timeout) &&
          result.error().is_operation(param::ErrorOperation::ReadCurrent),
          "read timeout is not an unknown mutation outcome");
}

void queued_read_cancellation() {
    auto instance = take(detail::Instance::create(unique_name()));
    detail::LockRegistry registry(instance);
    const auto shared_lock = take(registry.create());
    auto storage = take(wire::Storage::create(instance, shared_lock.name, shared_lock.id));
    Sessions sessions;
    detail::ModuleLocks locks(sessions.module, test_mid);
    detail::ModuleParams host(sessions.module, locks);
    host.identify("camera");
    ReadModule module;
    wire::ClientAccess::bind(module.params(), host);
    auto active = std::async(std::launch::async, [&] {
        return module.params().read_current("gain");
    });
    const auto attach = take(sessions.central->receive_until(deadline()));
    control::Bytes body;
    control::put_u32(body, wire::result_success);
    control::put_u32(body, wire::version);
    control::put_u64(body, shared_lock.id);
    control::put_u64(body, wire::storage_bytes);
    take(control::put_string(body, storage->descriptor().data_name));
    take(host.receive({control::Kind::Response, wire::domain, wire::Attach, attach.request, std::move(body)}));
    // The active reader is now in ModuleLocks, which has its own stop source.
    // Withhold its reply so stopping Param cannot release the active reader.
    const auto lock_request = take(sessions.central->receive_until(deadline()));
    check(lock_request.domain != wire::domain, "active reader is waiting for lock attachment");
    auto queued = std::async(std::launch::async, [&] {
        return module.params().read_current("gain");
    });
    host.stop();
    const auto cancellation_budget = config::param_lock_timeout / 2;
    const bool independent = queued.wait_for(cancellation_budget) == std::future_status::ready;
    // Always unblock the active task before assertions/exception unwinding.
    locks.stop();
    check(active.wait_for(test_timeout) == std::future_status::ready, "active lock read stops");
    check(!active.get(), "stopped lock acquisition returns failure");
    check(independent, "queued Param read cancels independently of active lock wait");
    const auto cancelled = queued.get();
    check(!cancelled && cancelled.error().is(param::ErrorCode::Cancelled), "queued read reports cancellation");
}

void storage_and_central_protocol() {
    auto instance = take(detail::Instance::create(unique_name()));
    detail::LockRegistry locks(instance);
    auto lock = take(locks.create());
    auto storage = take(wire::Storage::create(instance, lock.name, lock.id));
    const auto image = storage->descriptor();
    auto observer = take(vinc::concurrency::InterprocessMutex::open(lock.name, central_owner));
    auto memory = take(osal::SharedMemory::open(image.data_name, osal::SharedMemoryAccess::ReadOnly));
    auto mapping = take(memory.map(wire::storage_bytes, osal::SharedMemoryAccess::ReadOnly));
    control::Bytes buffer;
    buffer.reserve(config::param_slot_bytes);
    param::Snapshot snapshot;
    snapshot.revision = 1;
    snapshot.entries.emplace("camera.gain",
                             param::Entry{std::int32_t{5}, std::int32_t{1}, param::Permission::Public, 1, 10});
    take(storage->publish(snapshot));
    {
        auto guard = take(observer.try_lock());
        check(take(wire::copy_image(mapping.bytes(), guard.publication(), 0, buffer)) == 1,
              "published revision");
        take(guard.unlock());
    }
    check(take(take(wire::decode_snapshot(buffer)).entries.at("camera.gain").get<std::int32_t>()) == 5,
          "consistent full A/B image");
    snapshot.revision = 2;
    snapshot.entries.at("camera.gain").version = 2;
    snapshot.entries.at("camera.gain").value = std::int32_t{9};
    std::stop_source cancelled;
    cancelled.request_stop();
    check(!storage->publish(snapshot, cancelled.get_token()), "cancelled publication rejected");
    {
        auto guard = take(observer.try_lock());
        check(take(wire::copy_image(mapping.bytes(), guard.publication(), 1, buffer)) == 1 && buffer.empty(),
              "failed publication keeps previous slot");
    }
    // A fresh storage owns the manager's initial revision zero.
    storage.reset();
    storage = take(wire::Storage::create(instance, lock.name, lock.id));
    auto manager = take(wire::ParamManager::create(std::move(storage)));
    Sessions sessions;
    manager->admit(test_mid, "camera");
    std::uint64_t request_id = 0;
    const auto request = [&](control::Bytes payload, std::uint16_t operation = wire::Register) {
        ++request_id;
        take(manager->submit(
            test_mid, sessions.central,
            {control::Kind::Request, wire::domain, operation, request_id, std::move(payload)}));
        return take(sessions.module->receive_until(deadline()));
    };
    const auto registration = [](std::uint32_t version, std::string_view name, std::uint32_t permission,
                                  std::uint32_t restart_hint = 0) {
        control::Bytes result;
        control::put_u32(result, version);
        take(control::put_string(result, name));
        take(wire::put_value(result, std::int32_t{1}));
        control::put_u32(result, permission);
        control::put_u32(result, restart_hint);
        return result;
    };
    for (auto bytes :
         {registration(wire::version - 1, "gain", 1),
          registration(wire::version + 1, "gain", 2),
          registration(wire::version, "other.gain", 2),
          registration(wire::version, "gain", 0),
          registration(wire::version, "gain", 3),
          registration(wire::version, "gain", 2, 3),
          registration(wire::version, "gain", 2, UINT32_MAX)}) {
        const auto reply = request(std::move(bytes));
        check(take(wire::decode_response(reply.payload)).failure.has_value(),
              "central rejects malformed registration");
        check(take(manager->inspect()).entries.empty(), "rejected registration publishes nothing");
    }
    auto truncated_hint = registration(wire::version, "gain", 2, 1);
    truncated_hint.pop_back();
    check(take(wire::decode_response(request(std::move(truncated_hint)).payload)).failure.has_value(),
          "central rejects truncated restart hint");
    check(take(manager->inspect()).entries.empty(), "truncated registration publishes nothing");
    auto reply = request(registration(wire::version, "gain", 2, 1));
    check(!take(wire::decode_response(reply.payload)).failure, "valid registration after rejected requests");
    check(take(manager->inspect()).entries.at("camera.gain").restart_hint == param::RestartHint::Module,
          "central receives Module hint");
    const auto mutation = [](std::string_view name, std::int32_t value) {
        control::Bytes result;
        control::put_u32(result, wire::version);
        take(control::put_string(result, name));
        take(wire::put_value(result, value));
        return result;
    };
    const auto written = request(mutation("camera.gain", 7), wire::Set);
    check(!take(wire::decode_response(written.payload)).failure, "Limited owner writes");
    take(manager->command({"set", "camera.gain", "8"}));
    check(take(take(manager->inspect()).entries.at("camera.gain").get<std::int32_t>()) == 8,
          "central writes Limited values");
    take(manager->register_framework("limit", std::int32_t{1}));
    const auto framework_write = request(mutation("vinc.limit", 9), wire::Set);
    const auto framework_failure = take(wire::decode_response(framework_write.payload)).failure;
    check(framework_failure && framework_failure->is(param::ErrorCode::PermissionDenied),
          "Module cannot claim central ownership");

    Sessions foreign;
    constexpr MID foreign_mid = test_mid + 1;
    manager->admit(foreign_mid, "cameraExtra");
    std::uint64_t foreign_request = 0;
    const auto foreign_set = [&](std::string_view name, std::int32_t value) {
        take(manager->submit(foreign_mid, foreign.central,
            {control::Kind::Request, wire::domain, wire::Set, ++foreign_request, mutation(name, value)}));
        return take(wire::decode_response(take(foreign.module->receive_until(deadline())).payload));
    };
    const auto denied = foreign_set("camera.gain", 99);
    check(denied.failure && denied.failure->is(param::ErrorCode::PermissionDenied),
          "similar name does not grant Limited ownership");
    check(take(take(manager->inspect()).entries.at("camera.gain").get<std::int32_t>()) == 8,
          "denied write leaves value intact");
    check(!take(wire::decode_response(request(registration(wire::version, "shared", 1, 2)).payload)).failure,
          "Public registration");
    check(!foreign_set("camera.shared", 42).failure, "Public permits foreign writes");
    check(take(manager->inspect()).entries.at("camera.shared").restart_hint == param::RestartHint::App,
          "foreign Public write cannot change registration metadata");
    const auto wrong_session = manager->submit(test_mid, foreign.central,
        {control::Kind::Request, wire::domain, wire::Set, request_id + 1, mutation("camera.gain", 99)});
    check(!wrong_session && wrong_session.error().is(param::ErrorCode::PermissionDenied),
          "admitted MID cannot switch authenticated Sessions");
    control::Bytes oversized(wire::request_max_bytes + 1);
    auto oversize_result = manager->submit(
        test_mid, sessions.central,
        {control::Kind::Request, wire::domain, wire::Register, request_id + 1, std::move(oversized)});
    check(!oversize_result && oversize_result.error().is(param::ErrorCode::Protocol),
          "oversize request rejected before queue");
    const auto replay = manager->submit(test_mid, sessions.central,
                                        {control::Kind::Request, wire::domain, wire::Register, request_id,
                                         registration(wire::version, "other", 2)});
    check(!replay && replay.error().is(param::ErrorCode::Protocol), "replayed request ID rejected");

    // Hold publication so an earlier foreign write blocks the worker. The owner
    // request remains queued until after its MID has been retired; no sleep race.
    constexpr std::uint64_t blocking_owner = 100;
    auto blocker = take(vinc::concurrency::InterprocessMutex::open(lock.name, blocking_owner));
    auto publication_guard = take(blocker.try_lock());
    take(manager->submit(foreign_mid, foreign.central,
        {control::Kind::Request, wire::domain, wire::Set, ++foreign_request, mutation("camera.shared", 43)}));
    take(manager->submit(test_mid, sessions.central,
        {control::Kind::Request, wire::domain, wire::Set, ++request_id, mutation("camera.gain", 99)}));
    manager->exited(test_mid);
    take(publication_guard.unlock());
    const auto foreign_reply = take(foreign.module->receive_until(deadline()));
    check(!take(wire::decode_response(foreign_reply.payload)).failure, "earlier admitted writer finishes");
    const auto retired_reply = take(sessions.module->receive_until(deadline()));
    const auto retired_failure = take(wire::decode_response(retired_reply.payload)).failure;
    check(retired_failure && retired_failure->is(param::ErrorCode::Closed),
          "queued request rechecks retired MID before mutation");
    check(take(take(manager->inspect()).entries.at("camera.gain").get<std::int32_t>()) == 8,
          "retired queued mutation never publishes");
    check(take(manager->inspect()).entries.contains("camera.gain"), "exit retains values");
    check(!manager->submit(test_mid, sessions.central,
                           {control::Kind::Request, wire::domain, wire::Attach, ++request_id, {}}),
          "retired MID rejected");
    constexpr MID replacement_mid = foreign_mid + 1;
    Sessions replacement;
    manager->admit(replacement_mid, "camera");
    std::uint64_t replacement_request = 0;
    const auto replace_request = [&](std::uint16_t operation, control::Bytes payload) {
        take(manager->submit(replacement_mid, replacement.central,
            {control::Kind::Request, wire::domain, operation, ++replacement_request, std::move(payload)}));
        return take(wire::decode_response(take(replacement.module->receive_until(deadline())).payload));
    };
    const auto before_registration = replace_request(wire::Set, mutation("camera.gain", 20));
    check(before_registration.failure && before_registration.failure->is(param::ErrorCode::PermissionDenied),
          "new MID must register before acquiring Limited ownership");
    check(!replace_request(wire::Register, registration(wire::version, "gain", 2)).failure,
          "new incarnation registers retained parameter");
    check(!replace_request(wire::Set, mutation("camera.gain", 21)).failure,
          "replacement owner can write");
    const auto stale_write = manager->submit(test_mid, sessions.central,
        {control::Kind::Request, wire::domain, wire::Set, ++request_id, mutation("camera.gain", 99)});
    check(!stale_write && stale_write.error().is(param::ErrorCode::PermissionDenied),
          "retired Session cannot write replacement state");
    check(take(take(manager->inspect()).entries.at("camera.gain").get<std::int32_t>()) == 21,
          "stale request leaves replacement value intact");
    check(take(manager->inspect()).entries.at("camera.gain").restart_hint == param::RestartHint::None,
          "replacement registration can clear the previous hint");
    take(manager->register_framework("port", std::uint16_t{8080}, param::RestartHint::App));
    check(take(manager->inspect()).entries.at("vinc.port").restart_hint == param::RestartHint::App,
          "central parameters can declare App restart");
    const auto invalid_central = manager->register_framework("invalid", std::int32_t{1}, param::RestartHint::Module);
    check(!invalid_central && invalid_central.error().is(param::ErrorCode::InvalidValue),
          "central has no owning Module to restart");

}
}

int main() {
    try {
        queue_capacity();
        registration_options();
        current_read_errors();
        current_read_cancellation();
        queued_read_cancellation();
        current_read_timeout();
        host_protocol();
        storage_and_central_protocol();
        std::cout << "PASS: Param descriptor, causes, cancellation, A/B and request validation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
