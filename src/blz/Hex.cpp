#include "blz/Hex.hpp"

namespace blz {

// ref: FUN_142366260
uint64_t hex_encode(char* out, uint64_t outSize, const uint8_t* data, uint64_t length) {
    static const char s_digits[] = "0123456789abcdef";
    if (out && outSize != 0) {
        char* last = out + outSize - 1;
        char* at = out;
        for (const uint8_t* byte = data; at < last && static_cast<uint64_t>(byte - data) < length; byte++) {
            *at = s_digits[*byte >> 4];
            if (at + 1 == last) {
                at++;
                break;
            }
            at[1] = s_digits[*byte & 0xF];
            at += 2;
        }
        *at = '\0';
    }
    return length * 2;
}

// ref: FUN_142366100
uint64_t hex_decode(uint8_t* out, uint64_t outSize, const char* text, uint64_t length) {
    if (!text || length == 0) {
        return 0;
    }
    auto value = [](char c, uint8_t* digit) {
        if (static_cast<uint8_t>(c - '0') <= 9) {
            *digit = static_cast<uint8_t>(c - '0');
        } else if (static_cast<uint8_t>(c - 'a') < 6) {
            *digit = static_cast<uint8_t>(c - 0x57);
        } else if (static_cast<uint8_t>(c - 'A') < 6) {
            *digit = static_cast<uint8_t>(c - 0x37);
        } else {
            return false;
        }
        return true;
    };
    const char* p = text;
    for (uint64_t i = 0; i < outSize; i++) {
        if (length - 1 <= static_cast<uint64_t>(p - text) || *p == '\0') {
            break;
        }
        uint8_t high;
        uint8_t low;
        if (!value(p[0], &high) || !value(p[1], &low)) {
            break;
        }
        out[i] = static_cast<uint8_t>(high * 16 + low);
        p += 2;
    }
    return static_cast<uint64_t>(p - text) / 2;
}

}
