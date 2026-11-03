// Scripted lifecycle peer, not a Plugin loader. The real Control Service owns
// heartbeat while test gates hold reports; native loading is tested separately.
#include "plugin_test_support.hpp"
#include <vinc/control/channel.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/module_runtime.hpp>

namespace {
using namespace vinc;
using namespace vinc::test::plugin;

void report(control::Session& session, std::uint64_t request, std::uint32_t role, Error error) {
    control::Frame frame{control::Kind::Request, detail::lifecycle, detail::report_failure, request, {}};
    control::put_u32(frame.payload, role);
    const auto encoded = detail::encode_error(error);
    frame.payload.insert(frame.payload.end(), encoded.begin(), encoded.end());
    check(bool(session.send(std::move(frame))), "enqueue scripted failure");
    const auto deadline = Clock::now() + wait_budget;
    for (;;) {
        auto response = take(session.receive_until(deadline));
        // The peer deliberately survives Stop until the controller verifies the
        // stored primary and cleanup diagnostics. Every gate remains bounded.
        if (response.kind == control::Kind::Event && response.domain == detail::lifecycle &&
            response.operation == detail::stop) {
            continue;
        }
        check(response.kind == control::Kind::Response && response.domain == detail::lifecycle &&
              response.operation == detail::report_failure && response.request == request && response.payload.empty(),
              "central acknowledged exact failure request");
        return;
    }
}

int run(const std::vector<std::string>& args) {
    using namespace vinc::detail::bootstrap;
    check(args.size() == argument_count + 1, "gate directory argument required");
    const auto directory = take(osal::path_from_utf8(args[argument_count]));
    auto service = take(control::Service::create());
    auto socket = take(osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream));
    check(bool(socket.connect_until(osal::LocalEndpoint{args[endpoint]}, Clock::now() + wait_budget)), "connect peer");
    control::Options options;
    options.role = control::Role::Module;
    options.generation = std::stoull(args[generation]);
    options.token = args[token];
    options.heartbeat_interval = std::chrono::milliseconds(std::stoll(args[heartbeat_interval]));
    options.heartbeat_timeout = std::chrono::milliseconds(std::stoll(args[heartbeat_timeout]));
    options.handshake_timeout = std::chrono::milliseconds(std::stoll(args[startup_timeout]));
    auto session = take(service->attach(std::move(socket), options));
    check(bool(session->wait_open(Clock::now() + wait_budget)), "peer handshake");
    signal_file(directory / "connected");
    wait_file(directory / "first", Clock::now() + wait_budget);
    report(*session, 2, detail::primary_failure,
           wrap_error(module_system::ErrorCode::HookFailed, module_system::ErrorOperation::ConstructModule,
                      Error{module_system::ErrorCode::InvalidFactory, module_system::ErrorOperation::ConstructModule}));
    signal_file(directory / "first-acknowledged");
    wait_file(directory / "later", Clock::now() + wait_budget);
    report(*session, 3, detail::primary_failure,
           Error{module_system::ErrorCode::ChannelFailed, module_system::ErrorOperation::Receive});
    report(*session, 4, detail::cleanup_failure,
           Error{module_system::ErrorCode::HookException, module_system::ErrorOperation::StopHook});
    signal_file(directory / "later-acknowledged");
    wait_file(directory / "exit", Clock::now() + wait_budget);
    return detail::module_application_failure_exit;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
#ifdef _WIN32
            args.push_back(take(vinc::osal::path_to_utf8(std::filesystem::path(argv[index]))));
#else
            args.emplace_back(argv[index]);
#endif
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
