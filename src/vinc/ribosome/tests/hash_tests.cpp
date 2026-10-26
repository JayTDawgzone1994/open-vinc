// Known-answer SHA-256 tests, including incremental updates.
// These protect schema fingerprint reproducibility independently of generator output formatting.

#include <ribosome/sha256.hpp>
#include <iostream>
#include <stdexcept>
using vinc::crypto::SHA256;

namespace {
void test_known_answers() {
    for (const auto& [input, expected] : std::vector<std::pair<std::string, std::string>>{
             {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
             {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
             {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
             {std::string(1000000, 'a'),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}}) {
        for (std::size_t chunk :
             {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{65}, input.size() + 1}) {
            SHA256 hash;
            for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
                hash.update(input.substr(offset, chunk));
            }

            if (SHA256::to_hex(hash.digest()) != expected) {
                throw std::runtime_error("SHA-256 test vector/chunk mismatch");
            }

            if (SHA256::to_hex(hash.digest()) !=
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
                throw std::runtime_error("digest did not reset");
            }
        }
    }
}

// Fixed independent digests cover the one/two-block padding boundary and binary bytes.
void test_padding_boundaries() {
    const std::vector<std::pair<std::string, std::string>> cases{
        {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {std::string(65, 'a'), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {std::string("\0\x80\xff", 3), "5240672d7b51756b829ad0ef8d9468b7a078afa2f410484fd3892dab47becb72"},
    };
    for (const auto& [input, expected] : cases) {
        SHA256 hash;
        for (const char byte : input) {
            hash.update(std::string(1, byte));
        }

        if (SHA256::to_hex(hash.digest()) != expected) {
            throw std::runtime_error("SHA-256 padding/binary mismatch");
        }
    }
}
}

int main() {
    try {
        test_known_answers();
        test_padding_boundaries();
        std::cout << "PASS: SHA-256 known vectors, chunk boundaries and reuse\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
