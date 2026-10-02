// Deliberately defective peer: a valid handshake and Ready, then no heartbeat,
// even though its socket and process remain alive. Only used by integration tests.
#include <vinc/control/channel.hpp>
#include <thread>
#include <array>
int main(int argc, char** argv) {
    if (argc < 11) return 64;
    using namespace vinc;
    using namespace std::chrono_literals;
    auto socket = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
    if (!socket || !socket->connect_until(osal::LocalEndpoint{argv[2]}, control::Clock::now() + 3s)) return 65;
    control::Frame hello{control::Kind::Hello};
    control::put_u32(hello.payload, static_cast<std::uint32_t>(control::Role::Module));
    control::put_u64(hello.payload, std::stoull(argv[5])); control::put_string(hello.payload, argv[4]);
    control::put_u32(hello.payload, static_cast<std::uint32_t>(std::stoul(argv[6])));
    control::put_u32(hello.payload, static_cast<std::uint32_t>(std::stoul(argv[7])));
    auto send = [&](const control::Frame& frame) {
        auto bytes = control::encode(frame); std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto sent = socket->send_until(std::span<const std::byte>(bytes).subspan(offset), control::Clock::now() + 2s);
            if (!sent || !*sent) return false;
            offset += *sent;
        }
        return true;
    };
    if (!send(hello)) return 66;
    control::Frame claim{control::Kind::Request, 1, 3, 1}; control::put_string(claim.payload, argv[3]);
    if (!send(claim)) return 66;
    bool approved = false; control::Decoder decoder; std::array<std::byte, 1024> buffer;
    while (!approved) {
        auto got = socket->receive_until(buffer, control::Clock::now() + 2s);
        if (!got || got->eof) return 66;
        auto frames = decoder.feed(std::span<const std::byte>(buffer).first(got->bytes)); if (!frames) return 66;
        for (const auto& frame : *frames) if (frame.kind == control::Kind::Response && frame.domain == 1 && frame.operation == 3) {
            control::Reader reader(frame.payload); if (reader.u32() != 0 || reader.string() != argv[3] || !reader.done()) return 66;
            approved = true;
        }
    }
    control::Frame ready{control::Kind::Event, 1, 1}; control::put_string(ready.payload, argv[3]);
    if (!send(ready)) return 66;
    std::this_thread::sleep_for(2min);
    return 99;
}
