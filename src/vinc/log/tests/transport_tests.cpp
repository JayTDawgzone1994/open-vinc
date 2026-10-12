#include "support.hpp"
#include <vinc/log/detail/transport.hpp>
#include <vinc/osal/path.hpp>
#include <array>
#include <future>
#include <iostream>
using namespace vinc;
using namespace log_test;

namespace {
void send(osal::Socket& socket, const control::Frame& frame) {
    const auto encoded = take(control::encode(frame));
    std::span<const std::byte> remaining = encoded;
    while (!remaining.empty()) {
        const auto written = take(socket.send_until(remaining, deadline()));
        check(written != 0, "fixture send progresses");
        remaining = remaining.subspan(written);
    }
}

struct Receiver {
    osal::Socket socket;
    control::Decoder decoder;
};
}

int main() {
    try {
        Directory directory;
        const auto endpoint = take(osal::path_to_utf8(directory.path / "transport"));
        auto listener = take(osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream));
        take(listener.bind(osal::LocalEndpoint{endpoint + ".log"}));
        take(listener.listen(1));
        control::Options options;
        options.role = control::Role::Module;
        options.generation = 7;
        options.token = "log-backpressure-test";
        options.heartbeat_timeout_enabled = false;
        auto receive = std::async(std::launch::async, [&] {
            Receiver receiver{take(listener.accept_until(deadline())), {}};
            bool hello = false;
            while (!hello) {
                std::array<std::byte, 4096> bytes;
                auto got = take(receiver.socket.receive_until(bytes, deadline()));
                check(!got.eof, "handshake not EOF");
                for (const auto& frame : take(receiver.decoder.feed(std::span(bytes).first(got.bytes)))) {
                    hello = hello || frame.kind == control::Kind::Hello;
                }
            }
            control::Frame response{control::Kind::Hello};
            control::put_u32(response.payload, static_cast<std::uint32_t>(control::Role::Central));
            control::put_u64(response.payload, options.generation);
            take(control::put_string(response.payload, options.token + ":log"));
            control::put_u32(response.payload,
                             static_cast<std::uint32_t>(options.heartbeat_interval.count()));
            control::put_u32(response.payload, static_cast<std::uint32_t>(options.heartbeat_timeout.count()));
            control::put_u32(response.payload, control::hello_version);
            control::put_u32(response.payload, 0);
            send(receiver.socket, response);
            return receiver;
        });
        auto service = take(control::Service::create());
        auto host = log::detail::Host::connect(*service, endpoint, options);
        auto receiver = receive.get();
        // Deliberately stop receiving: both kernel and framework queues are bounded.
        constexpr std::size_t maximum_attempts = 20000;
        const std::string text(config::log_message_bytes, 'x');
        bool rejected = false;
        for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
            auto result = host->write(log::Level::Info, text);
            if (!result) {
                check(result.error().is(log::ErrorCode::Backpressure), "expected bounded send backpressure");
                rejected = true;
                break;
            }
        }
        check(rejected, "paused receiver saturates sender");
        bool statistics = false;
        const auto until = deadline();
        while (!statistics && std::chrono::steady_clock::now() < until) {
            host->poll(); // No new application writes after the rejected record.
            std::array<std::byte, 16384> bytes;
            auto got = take(receiver.socket.receive_until(bytes, until));
            check(!got.eof, "receiver stays connected");
            for (const auto& frame : take(receiver.decoder.feed(std::span(bytes).first(got.bytes)))) {
                if (frame.domain == log::detail::domain &&
                    frame.operation == log::detail::statistics_operation) {
                    control::Reader reader(frame.payload);
                    const auto version = reader.u32();
                    const auto count = reader.u64();
                    statistics = version == log::detail::protocol_version && count != 0 && reader.done();
                }
            }
        }
        check(statistics, "watchdog statistics delivered without another application record");
        std::cout << "Log transport tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
