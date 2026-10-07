#pragma once

#include <cstdint>
#include <string_view>

namespace vinc::osal::detail {
// Validate Unicode scalar encodings only. NUL policy belongs to the caller.
inline bool valid_utf8_encoding(std::string_view text) noexcept {
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    unsigned remaining = 0;
    for (const unsigned char byte : text) {
        if (!remaining) {
            if (byte < 0x80) {
                continue;
            }

            if (byte >= 0xc2 && byte <= 0xdf) {
                value = byte & 0x1f;
                minimum = 0x80;
                remaining = 1;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                value = byte & 0x0f;
                minimum = 0x800;
                remaining = 2;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                value = byte & 7;
                minimum = 0x10000;
                remaining = 3;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte & 0x3f);
            if (--remaining == 0 &&
                (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))) {
                return false;
            }
        }
    }

    return remaining == 0;
}
}
