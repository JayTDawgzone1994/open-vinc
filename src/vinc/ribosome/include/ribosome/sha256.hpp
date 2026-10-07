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
#include <type_traits>

namespace vinc::crypto {

class SHA256 {
public:
    using HashBytes = std::array<std::uint8_t, 32>;

    SHA256() {
        reset();
    }

    void update(const std::string& data) {
        // Read the string's object representation through unsigned char. On supported
        // targets uint8_t is that byte type, so no alignment or new-object lifetime is required.
        static_assert(std::is_same_v<std::uint8_t, unsigned char>);
        update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    void update(const std::uint8_t* data, std::size_t remaining_bytes) {
        total_bytes_ += remaining_bytes;
        while (remaining_bytes) {
            const auto count = std::min<std::size_t>(remaining_bytes, block_bytes - buffer_bytes_);
            std::copy_n(data, count, buffer_.begin() + buffer_bytes_);
            buffer_bytes_ += static_cast<std::uint32_t>(count);
            data += count;
            remaining_bytes -= count;
            if (buffer_bytes_ == block_bytes) {
                transform(buffer_.data());
                buffer_bytes_ = 0;
            }
        }
    }

    HashBytes digest() {
        HashBytes hash;
        std::uint8_t padding[block_bytes * 2];
        std::uint32_t padding_bytes = (buffer_bytes_ < length_offset)
                                          ? (length_offset - buffer_bytes_)
                                          : (block_bytes + length_offset - buffer_bytes_);

        std::fill(padding, padding + block_bytes * 2, std::uint8_t{0});
        padding[0] = 0x80;

        // Append the original bit count before update() consumes the padding bytes.
        std::uint64_t message_bits = total_bytes_ * 8;
        for (int i = 0; i < 8; i++) {
            padding[padding_bytes + i] = static_cast<std::uint8_t>(message_bits >> (56 - i * 8));
        }

        update(padding, padding_bytes + 8);

        for (int i = 0; i < 8; i++) {
            hash[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
            hash[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            hash[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            hash[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }

        reset(); // A completed digest resets this object for the next independent message.
        return hash;
    }

    static std::string to_hex(const HashBytes& hash) {
        std::stringstream hex_text;
        for (std::uint8_t b : hash) {
            hex_text << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
        }

        return hex_text.str();
    }

private:
    // SHA-256 format constants; these are algorithm requirements, not tuning settings.
    static constexpr std::uint32_t block_bytes = 64;
    static constexpr std::size_t state_words = 8;
    static constexpr std::size_t round_count = 64;
    static constexpr std::size_t input_words = 16;
    static constexpr std::uint32_t length_offset = 56;

    std::array<std::uint32_t, state_words> state_;
    std::array<std::uint8_t, block_bytes> buffer_;
    std::uint32_t buffer_bytes_;
    std::uint64_t total_bytes_;

    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    static std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    static std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    void transform(const std::uint8_t* data) {
        std::uint32_t schedule[round_count];
        for (std::size_t i = 0; i < input_words; i++) {
            schedule[i] = static_cast<std::uint32_t>(data[i * 4]) << 24 |
                          static_cast<std::uint32_t>(data[i * 4 + 1]) << 16 |
                          static_cast<std::uint32_t>(data[i * 4 + 2]) << 8 |
                          static_cast<std::uint32_t>(data[i * 4 + 3]);
        }

        for (std::size_t i = input_words; i < round_count; i++) {
            std::uint32_t sigma0 =
                rotr(schedule[i - 15], 7) ^ rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            std::uint32_t sigma1 =
                rotr(schedule[i - 2], 17) ^ rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + sigma0 + schedule[i - 7] + sigma1;
        }

        // Keep the conventional a..h working-word names to match the compression equations.
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        static const std::uint32_t round_constants[round_count] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        for (std::size_t i = 0; i < round_count; i++) {
            std::uint32_t sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t choice = choose(e, f, g);
            std::uint32_t temp1 = h + sum1 + choice + round_constants[i] + schedule[i];
            std::uint32_t sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t majority_bits = majority(a, b, c);
            std::uint32_t temp2 = sum0 + majority_bits;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    void reset() {
        state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        buffer_.fill(0);
        buffer_bytes_ = 0;
        total_bytes_ = 0;
    }
};

} // namespace vinc::crypto
