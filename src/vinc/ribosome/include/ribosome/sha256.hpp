#pragma once

// Incremental SHA-256 used to derive deterministic schema fingerprints.
// Ribosome truncates the digest for type identification; this is not message authentication.

#include <vector>
#include <string>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <array>
#include <algorithm>

namespace vinc::crypto {

class SHA256 {
public:
    using HashBytes = std::array<uint8_t, 32>;

    SHA256() { reset(); }

    void update(const std::string& data) {
        update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    void update(const uint8_t* data, size_t len) {
        _total_len += len;
        while (len) {
            const auto count = std::min<std::size_t>(len, 64 - _buffer_len);
            std::copy_n(data, count, _buffer.begin() + _buffer_len);
            _buffer_len += static_cast<std::uint32_t>(count);
            data += count; len -= count;
            if (_buffer_len == 64) { transform(_buffer.data()); _buffer_len = 0; }
        }
    }

    HashBytes digest() {
        HashBytes hash;
        uint8_t pad[128];
        uint32_t pad_len = (_buffer_len < 56) ? (56 - _buffer_len) : (120 - _buffer_len);
        
        std::fill(pad, pad + 128, uint8_t{0});
        pad[0] = 0x80;
        
        uint64_t bits = _total_len * 8;
        for (int i = 0; i < 8; i++) {
            pad[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
        }

        update(pad, pad_len + 8);

        for (int i = 0; i < 8; i++) {
            hash[i * 4]     = (uint8_t)(_state[i] >> 24);
            hash[i * 4 + 1] = (uint8_t)(_state[i] >> 16);
            hash[i * 4 + 2] = (uint8_t)(_state[i] >> 8);
            hash[i * 4 + 3] = (uint8_t)(_state[i]);
        }
        
        reset(); // 算完自動重置
        return hash;
    }

    static std::string to_hex(const HashBytes& hash) {
        std::stringstream ss;
        for (uint8_t b : hash) {
            ss << std::setw(2) << std::setfill('0') << std::hex << (int)b;
        }
        return ss.str();
    }

private:
    std::array<uint32_t, 8> _state;
    std::array<uint8_t, 64> _buffer;
    uint32_t _buffer_len;
    uint64_t _total_len;

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t choose(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static uint32_t majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

    void transform(const uint8_t* data) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16 |
                   (uint32_t)data[i * 4 + 2] << 8  | (uint32_t)data[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3],
                 e = _state[4], f = _state[5], g = _state[6], h = _state[7];

        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = choose(e, f, g);
            uint32_t temp1 = h + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = majority(a, b, c);
            uint32_t temp2 = S0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
        _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
    }

    void reset() {
        _state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        _buffer.fill(0);
        _buffer_len = 0;
        _total_len = 0;
    }
};

} // namespace vinc::crypto
