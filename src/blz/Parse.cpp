#include "blz/Parse.hpp"

namespace blz {

namespace {

bool IsSpace(char c) {
    return c == ' ' || static_cast<uint8_t>(c - 9) < 5;
}

}

// ref: FUN_1423634b0
uint64_t parse_uint32(const char* text, uint64_t length, uint32_t* value, bool* error) {
    *value = 0;
    *error = false;
    if (length == 0) {
        return 0;
    }
    uint64_t at = 0;
    char first;
    while ((first = text[at]), IsSpace(first)) {
        at++;
        if (length <= at) {
            return 0;
        }
    }
    if (length <= at) {
        return 0;
    }
    uint64_t start = at + (((first - '+') & 0xFD) == 0 ? 1 : 0);
    const char* digits = text + start;
    const char* p = digits;
    uint32_t result = 0;
    bool overflow = false;
    while (p < text + length) {
        uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (digit > 9) {
            break;
        }
        uint32_t next = digit + result * 10;
        if (next < result) {
            overflow = true;
        }
        result = next;
        p++;
    }
    uint64_t count = static_cast<uint64_t>(p - digits);
    if (count == 0) {
        return 0;
    }
    if (first == '-') {
        if (overflow || result != 0) {
            *error = true;
        }
        *value = 0;
        return start + count;
    }
    if (overflow) {
        result = 0xFFFFFFFF;
        *error = true;
    }
    *value = result;
    return start + count;
}

}
