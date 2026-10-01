// Known-answer SHA-256 tests, including incremental updates.
// These protect schema fingerprint reproducibility independently of generator output formatting.

#include <ribosome/sha256.hpp>
#include <iostream>
#include <stdexcept>
using vinc::crypto::SHA256;
int main() {
    try {
        for (const auto& [input, expected] : std::vector<std::pair<std::string, std::string>>{
            {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
            {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
            {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}
        }) {
            for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{65}, input.size() + 1}) {
                SHA256 hash;
                for (std::size_t offset = 0; offset < input.size(); offset += chunk) hash.update(input.substr(offset, chunk));
                if (SHA256::to_hex(hash.digest()) != expected) throw std::runtime_error("SHA-256 test vector/chunk mismatch");
                if (SHA256::to_hex(hash.digest()) != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") throw std::runtime_error("digest did not reset");
            }
        }
        std::cout << "PASS: SHA-256 known vectors, chunk boundaries and reuse\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
