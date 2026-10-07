#include <vinc/control/channel.hpp>
// A parent supplies controlled Bus responses over a real Socket session. The child
// exercises Client registration and the runtime dispatcher in an independent process.
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/osal/process.hpp>
#include <vinc_messages/Frame.hpp>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
namespace bus = vinc::message_bus;
namespace protocol = vinc::message_bus::detail;
constexpr auto operation_timeout = 10s;
constexpr auto cleanup_timeout = 3s;
constexpr auto fixture_heartbeat_interval = 100ms;
constexpr auto fixture_heartbeat_timeout = 5s;
constexpr std::size_t listener_backlog = 1;
constexpr std::size_t session_capacity = 1;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void ok(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

Deadline deadline() {
    return control::Clock::now() + operation_timeout;
}

std::array<Error, 4> fixture_errors() {
    // Foreign native values are synthetic: test numeric provenance, not OS text equivalence.
    const NativeError native{static_cast<NativeErrorSource>(9001), -9002};
    const Error os_error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::CreateSharedMemory, native};
    const auto normal = wrap_error(bus::ErrorCode::Io, bus::ErrorOperation::CreateControl, os_error);
    const auto unknown = Error::from_raw(ErrorDomain::MessageBus, -9003, -9004, native);
    auto truncated = normal;
    for (std::size_t index = 0; index < config::error_chain_max_depth; ++index) {
        truncated = wrap_error(bus::ErrorCode::Io, bus::ErrorOperation::Advertise, truncated);
    }
    const Error protocol_failure{bus::ErrorCode::Protocol, bus::ErrorOperation::Subscribe};
    return {normal, unknown, truncated, protocol_failure};
}

control::Options options(control::Role role) {
    control::Options result;
    result.role = role;
    result.generation = 1;
    result.token = "bus-error-fixture";
    result.heartbeat_interval = fixture_heartbeat_interval;
    result.heartbeat_timeout = fixture_heartbeat_timeout;
    return result;
}

osal::Socket open_listener() {
    auto listener = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    ok(listener.bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0}));
    ok(listener.listen(listener_backlog));
    return listener;
}

std::shared_ptr<control::Session> connect_session(control::Service& service, std::uint16_t port) {
    auto socket = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
    ok(socket.connect_until(osal::IpEndpoint{osal::IPv4Address::loopback(), port}, deadline()));
    auto session = take(service.attach(std::move(socket), options(control::Role::Module)));
    ok(session->wait_open(deadline()));
    return session;
}

int child(std::uint16_t port) {
    auto service = take(control::Service::create(session_capacity));
    auto session = connect_session(*service, port);
    auto client = take(protocol::RuntimeAccess::create(session));
    const auto oversized = client->advertise<Frame>(std::string(control::max_payload + 1, 'x'));
    check(!oversized && oversized.error().is(bus::ErrorCode::InvalidArgument) &&
              oversized.error().is_operation(bus::ErrorOperation::Advertise) &&
              oversized.error().causes().size() == 1 &&
              oversized.error().causes().front().domain == ErrorDomain::ControlChannel,
          "local request encoding lost Control Channel cause");

    for (const auto& expected : fixture_errors()) {
        // Use Subscribe to cover failure decoding before any topology is installed.
        auto pending = std::async(std::launch::async, [&] {
            return client->subscribe<Frame>("error-fixture");
        });
        auto frame = take(session->receive_until(deadline()));
        ok(protocol::RuntimeAccess::receive(*client, std::move(frame)));
        check(pending.wait_until(deadline()) == std::future_status::ready, "registration did not wake");
        const auto response = pending.get();
        check(!response && response.error() == expected, "Client lost remote error fields or causes");
        check(error_to_string(response.error()).contains("MessageBus"), "child catalog unavailable");
        check(protocol::RuntimeAccess::inspect_attachments(*client).empty(), "failed registration mutated topology");
    }

    auto old_event = take(session->receive_until(deadline()));
    auto rejected = protocol::RuntimeAccess::receive(*client, std::move(old_event));
    check(!rejected && rejected.error().is(bus::ErrorCode::Protocol), "runtime accepted v1 topology event");
    return 0;
}

// Keep the child under ownership even if accept, handshake or a response check fails.
struct ChildOwner {
    osal::Process process;
    bool exited = false;

    ~ChildOwner() {
        if (!exited) {
            (void)process.terminate();
            (void)process.wait_until(control::Clock::now() + cleanup_timeout);
        }
    }
};

osal::Process launch_child(osal::ProcessService& processes, const std::filesystem::path& executable,
                           std::uint16_t port) {
    osal::SpawnOptions spawn;
    spawn.executable = executable;
    spawn.arguments = {"--client", std::to_string(port)};
    return take(processes.spawn(spawn));
}

void parent(const std::filesystem::path& executable) {
    auto listener = open_listener();
    const auto address = take(listener.local_address());
    const auto port = std::get<osal::IpEndpoint>(address).port;
    auto service = take(control::Service::create(session_capacity));
    auto processes = take(osal::ProcessService::create());
    ChildOwner child_owner{launch_child(processes, executable, port)};

    auto socket = take(listener.accept_until(deadline()));
    auto session = take(service->attach(std::move(socket), options(control::Role::Central)));
    ok(session->wait_open(deadline()));
    for (const auto& failure : fixture_errors()) {
        const auto request = take(session->receive_until(deadline()));
        check(request.kind == control::Kind::Request && request.domain == protocol::message_bus_domain &&
                  request.operation == protocol::Subscribe, "expected v2 registration");
        control::Frame response{control::Kind::Response, request.domain, request.operation, request.request,
                                protocol::encode_failure(failure)};
        ok(session->send(std::move(response)));
    }

    // v1 Detach (6) must never be interpreted as v2 Detach (0x0206).
    control::Frame old_event{control::Kind::Event, protocol::message_bus_domain, 6};
    control::put_u64(old_event.payload, 1);
    control::put_u64(old_event.payload, 1);
    ok(session->send(std::move(old_event)));
    const auto exit = take(child_owner.process.wait_until(deadline()));
    child_owner.exited = true;
    check(exit.value == 0, "child error transport checks failed");
    ok(processes.forget(child_owner.process.id()));
    ok(processes.shutdown_until(deadline()));
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--client") {
            return child(static_cast<std::uint16_t>(std::stoul(argv[2])));
        }
        check(argc == 2, "expected executable path");
        parent(std::filesystem::absolute(argv[1]));
        std::cout << "PASS: Bus errors cross processes through Socket and Client registration\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
