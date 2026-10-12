#include "support.hpp"
#include "../../control_channel/tests/channel_test_support.hpp"
#include <vinc/module/detail/module_params.hpp>
#include <vinc/module/detail/instance.hpp>
#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/param/detail/manager.hpp>
#include <future>
#include <iostream>

namespace {
using namespace vinc;
using namespace param_test;
namespace wire = vinc::param::detail;
constexpr MID test_mid = 7;
constexpr std::uint64_t unrelated_lock = 99;

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
    for (auto bytes : {descriptor(wire::version + 1), descriptor(wire::version, wire::storage_bytes + 1)}) {
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
                             param::Entry{std::int32_t{5}, std::int32_t{1}, param::Permission::Open, 1, 10});
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
    const auto request = [&](control::Bytes payload) {
        ++request_id;
        take(manager->submit(
            test_mid, sessions.central,
            {control::Kind::Request, wire::domain, wire::Register, request_id, std::move(payload)}));
        return take(sessions.module->receive_until(deadline()));
    };
    const auto registration = [](std::uint32_t version, std::string_view name, std::uint32_t permission) {
        control::Bytes result;
        control::put_u32(result, version);
        take(control::put_string(result, name));
        take(wire::put_value(result, std::int32_t{1}));
        control::put_u32(result, permission);
        return result;
    };
    for (auto bytes :
         {registration(wire::version + 1, "gain", 0), registration(wire::version, "other.gain", 0),
          registration(wire::version, "gain", 2)}) {
        const auto reply = request(std::move(bytes));
        check(take(wire::decode_response(reply.payload)).failure.has_value(),
              "central rejects malformed registration");
        check(take(manager->inspect()).entries.empty(), "rejected registration publishes nothing");
    }
    auto reply = request(registration(wire::version, "gain", 0));
    check(!take(wire::decode_response(reply.payload)).failure, "valid registration after rejected requests");
    control::Bytes oversized(wire::request_max_bytes + 1);
    auto oversize_result = manager->submit(
        test_mid, sessions.central,
        {control::Kind::Request, wire::domain, wire::Register, request_id + 1, std::move(oversized)});
    check(!oversize_result && oversize_result.error().is(param::ErrorCode::Protocol),
          "oversize request rejected before queue");
    const auto replay = manager->submit(test_mid, sessions.central,
                                        {control::Kind::Request, wire::domain, wire::Register, request_id,
                                         registration(wire::version, "other", 0)});
    check(!replay && replay.error().is(param::ErrorCode::Protocol), "replayed request ID rejected");
    manager->exited(test_mid);
    check(take(manager->inspect()).entries.contains("camera.gain"), "exit retains values");
    check(!manager->submit(test_mid, sessions.central,
                           {control::Kind::Request, wire::domain, wire::Attach, ++request_id, {}}),
          "retired MID rejected");
}
}

int main() {
    try {
        host_protocol();
        storage_and_central_protocol();
        std::cout << "PASS: Param descriptor, causes, cancellation, A/B and request validation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
