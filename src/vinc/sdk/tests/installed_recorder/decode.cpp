#include <vinc_messages/RecorderSample.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }

    constexpr std::uint64_t record_count = 64;
    constexpr std::size_t payload_bytes = 32;
    for (std::uint64_t sequence = 1; sequence <= record_count; ++sequence) {
        const auto path = std::filesystem::path(argv[1]) / (std::to_string(sequence) + ".bin");
        std::ifstream input(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
        auto message = vinc::RecorderSample::from_bytes(std::as_bytes(std::span(bytes)));
        if (!message || (*message)->data().sequence != sequence ||
            (*message)->data().label.str() != "relocated recorder" ||
            (*message)->data().payload.size() != payload_bytes) {
            std::cerr << "Recorded image mismatch: " << sequence << '\n';
            return 1;
        }

        for (const auto byte : std::as_const((*message)->data().payload)) {
            if (byte != sequence) {
                return 1;
            }
        }
    }

    std::cout << "INSTALLED_RECORDER_CONTENT_OK\n";
    return 0;
}
