// Deliberately defective peer: a valid handshake and Ready, then no heartbeat,
// even though its socket and process remain alive. Only used by integration tests.
#include <vinc/control/channel.hpp>
#include <thread>
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
    control::Frame ready{control::Kind::Event, 1, 1}; control::put_string(ready.payload, argv[3]);
    if (!send(ready)) return 66;
    std::this_thread::sleep_for(2min);
    return 99;
}
