#include "os/Os.hpp"
#include <cctype>
#include <cstring>

// Dotted IPv4 into 4 bytes (network order): exactly four parts, each 0..255; false otherwise.
// ref: FUN_141238ff0
bool OsParseIPv4(const char* text, uint32_t* address) {
    uint8_t bytes[4] = { 0, 0, 0, 0 };
    uint8_t* part = bytes;
    int32_t parts = 0;
    bool inPart = false;
    if (*text == '\0') {
        return false;
    }
    for (char c = *text; c != '\0'; c = *text) {
        text++;
        if (static_cast<uint32_t>(c - '0') < 10) {
            int32_t value = c - '0' + *part * 10;
            if (value > 0xFF) {
                return false;
            }
            if (!inPart) {
                if (++parts > 4) {
                    return false;
                }
                inPart = true;
            }
            *part = static_cast<uint8_t>(value);
        } else {
            if (c != '.' || !inPart || parts == 4) {
                return false;
            }
            part++;
            inPart = false;
            *part = 0;
        }
    }
    if (parts < 4) {
        return false;
    }
    std::memcpy(address, bytes, sizeof(bytes));
    return true;
}

// Textual IPv6 into 16 bytes (BSD inet_pton6, with an embedded dotted IPv4 tail).
// ref: FUN_141239090
bool OsParseIPv6(const char* text, uint8_t address[16]) {
    uint8_t bytes[16] = {};
    uint8_t* tp = bytes;
    uint8_t* const end = bytes + 16;
    uint8_t* colon = nullptr;

    if (*text == ':') {
        if (*++text != ':') {
            return false;
        }
    }
    const char* token = text;
    bool sawDigit = false;
    uint32_t value = 0;
    int32_t digits = 0;
    int32_t ch = toupper(*text);
    if (ch == 0) {
        return false;
    }
    const char* p = text;
    for (;;) {
        p++;
        if (static_cast<uint32_t>(ch - '0') < 10 || static_cast<uint32_t>(ch - 'A') < 6) {
            if (digits > 3) {
                return false;
            }
            value = static_cast<uint32_t>(ch - (ch > '9' ? 0x37 : 0x30)) | (value << 4);
            if (value > 0xFFFF) {
                return false;
            }
            sawDigit = true;
            digits++;
        } else if (ch == ':') {
            token = p;
            if (!sawDigit) {
                if (colon) {
                    return false;
                }
                colon = tp;
            } else {
                if (*p == '\0' || tp + 2 > end) {
                    return false;
                }
                *tp++ = static_cast<uint8_t>(value >> 8);
                *tp++ = static_cast<uint8_t>(value);
                sawDigit = false;
                digits = 0;
                value = 0;
            }
        } else if (ch == '.') {
            if (tp + 4 > end) {
                return false;
            }
            uint32_t v4;
            if (!OsParseIPv4(token, &v4)) {
                return false;
            }
            std::memcpy(tp, &v4, 4);
            tp += 4;
            sawDigit = false;
            goto done;
        } else {
            return false;
        }
        ch = toupper(*p);
        if (ch == 0) {
            break;
        }
    }
    if (sawDigit) {
        if (tp + 2 > end) {
            return false;
        }
        *tp++ = static_cast<uint8_t>(value >> 8);
        *tp++ = static_cast<uint8_t>(value);
    }
done:
    if (colon) {
        int64_t n = tp - colon;
        for (int64_t i = 1; i <= n; i++) {
            end[-i] = colon[n - i];
            colon[n - i] = 0;
        }
    } else if (tp != end) {
        return false;
    }
    std::memcpy(address, bytes, 16);
    return true;
}

// A numeric address, IPv4 (family 1) or IPv6 (family 2).
// ref: FUN_1412389c0
bool OsParseAddress(OSNETADDRESS* address, const char* text) {
    uint32_t v4;
    if (OsParseIPv4(text, &v4)) {
        std::memcpy(address->m_bytes, &v4, 4);
        address->m_family = 1;
        return true;
    }
    uint8_t v6[16];
    if (OsParseIPv6(text, v6)) {
        std::memcpy(address->m_bytes, v6, 16);
        address->m_family = 2;
        return true;
    }
    return false;
}
