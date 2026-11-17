// Scripted peers exercise central admission without invoking user callbacks.
// The real Control Service still owns framing, FIFO, heartbeat and shutdown.
#include "plugin_test_support.hpp"
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <vinc/rpc/detail/protocol.hpp>

namespace {
using namespace vinc;
using namespace vinc::test::plugin;
constexpr std::uint64_t burst_request = 2000000;
constexpr std::uint64_t rpc_request = 3000000;
constexpr int burst_frames = 40;
constexpr int rpc_frames = 3;

int run(const std::vector<std::string>& args) {
    using namespace vinc::detail::bootstrap;
    check(args.size() == argument_count + 1, "scripted peer name required");
    const auto& name = args[argument_count];
    auto service = take(control::Service::create());
    auto socket = take(osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream));
    check(bool(socket.connect_until(osal::LocalEndpoint{args[endpoint]}, Clock::now() + wait_budget)),
          "connect scripted peer");
    control::Options options;
    options.role = control::Role::Module;
    options.generation = std::stoull(args[generation]);
    options.token = args[token];
    auto session = take(service->attach(std::move(socket), options));
    check(bool(session->wait_open(Clock::now() + wait_budget)), "scripted handshake");
    control::Frame claim{control::Kind::Request, detail::lifecycle, detail::register_name,
                         detail::name_registration_request, {}};
    check(bool(control::put_string(claim.payload, name)), "encode peer name");
    check(bool(session->send(std::move(claim))), "claim name");
    const auto approved = take(session->receive_until(Clock::now() + wait_budget));
    control::Reader approval(approved.payload);
    check(approved.operation == detail::register_name && approval.u32() == detail::result_success,
          "name approved");
    control::Frame ready{control::Kind::Event, detail::lifecycle, detail::ready, 0, {}};
    check(bool(control::put_string(ready.payload, name)), "encode ready");
    check(bool(session->send(std::move(ready))), "send ready");

    for (;;) {
        auto request = take(session->receive_until(Clock::now() + wait_budget));
        if (request.domain == detail::lifecycle && request.operation == detail::stop) {
            return 0;
        }
        check(request.domain == detail::commands, "scripted command domain");
        if (request.request == rpc_request) {
            for (int index = 0; index < rpc_frames; ++index) {
                // These frames are retained by the deliberately parked RPC
                // worker. This test verifies admission, not handler execution.
                check(bool(session->send(rpc::detail::frame(rpc::detail::Acquire,
                    rpc_request + index))), "enqueue RPC admission probe");
            }
            check(bool(session->send({control::Kind::Response, detail::commands, detail::execute,
                                      rpc_request + rpc_frames, {}})), "enqueue trailing FIFO marker");
            continue;
        }
        const int count = request.request == burst_request ? burst_frames : 1;
        for (int index = 0; index < count; ++index) {
            check(bool(session->send({control::Kind::Response, detail::commands, detail::execute,
                                      request.request + index, {}})), "enqueue scripted response");
        }
    }
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
            args.push_back(vinc::test::plugin::take(vinc::osal::path_to_utf8(std::filesystem::path(argv[index]))));
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
