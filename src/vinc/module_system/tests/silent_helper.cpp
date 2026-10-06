// Deliberately defective peer: a valid handshake and Ready, then no heartbeat,
// even though its socket and process remain alive. Only used by integration tests.
#include <vinc/control/channel.hpp>
#include <thread>
#include <array>

namespace {
using namespace std::chrono_literals;
constexpr auto connect_timeout = 3s;
constexpr auto frame_io_timeout = 2s;
constexpr auto simulated_hang_duration = 2min;
constexpr std::size_t receive_buffer_bytes = 1024;
}

int main(int argc, char** argv) {
    if (argc < 11) {
        return 64;
    }

    using namespace vinc;
    using namespace std::chrono_literals;
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket ||
        !socket->connect_until(osal::LocalEndpoint{argv[2]}, control::Clock::now() + connect_timeout)) {
        return 65;
    }

    control::Frame hello{control::Kind::Hello};
    control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Module));
    control::put_u64(hello.payload, std::stoull(argv[5]));
    if (!control::put_string(hello.payload, argv[4])) {
        return 66;
    }
    control::put_u32(hello.payload, static_cast<std::uint32_t>(std::stoul(argv[6])));
    control::put_u32(hello.payload, static_cast<std::uint32_t>(std::stoul(argv[7])));
    control::put_u32(hello.payload, control::hello_version);
    control::put_u32(hello.payload, 1u);
    auto send_frame = [&](const control::Frame& frame) {
        auto encoded = control::encode(frame);
        if (!encoded) {
            return false;
        }
        const auto& bytes = *encoded;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto sent = socket->send_until(std::span<const std::byte>(bytes).subspan(offset),
                                           control::Clock::now() + frame_io_timeout);
            if (!sent || !*sent) {
                return false;
            }

            offset += *sent;
        }
        return true;
    };
    if (!send_frame(hello)) {
        return 66;
    }

    control::Frame claim{control::Kind::Request, 1, 3, 1};
    if (!control::put_string(claim.payload, argv[3])) {
        return 66;
    }
    if (!send_frame(claim)) {
        return 66;
    }

    bool approved = false;
    control::Decoder decoder;
    std::array<std::byte, receive_buffer_bytes> buffer;
    while (!approved) {
        auto received = socket->receive_until(buffer, control::Clock::now() + frame_io_timeout);
        if (!received || received->eof) {
            return 66;
        }

        auto frames = decoder.feed(std::span<const std::byte>(buffer).first(received->bytes));
        if (!frames) {
            return 66;
        }

        for (const auto& frame : *frames) {
            if (frame.kind == control::Kind::Response && frame.domain == 1 && frame.operation == 3) {
                control::Reader reader(frame.payload);
                if (reader.u32() != 0 || reader.string() != argv[3] || !reader.done()) {
                    return 66;
                }

                approved = true;
            }
        }
    }
    control::Frame ready{control::Kind::Event, 1, 1};
    if (!control::put_string(ready.payload, argv[3])) {
        return 66;
    }
    if (!send_frame(ready)) {
        return 66;
    }

    std::this_thread::sleep_for(simulated_hang_duration);
    return 99;
}
