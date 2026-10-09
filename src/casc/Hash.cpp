#include "casc/Hash.hpp"

namespace casc {

namespace {

uint32_t Rot(uint32_t x, uint32_t k) {
    return (x << k) | (x >> (32 - k));
}

void Mix(uint32_t& a, uint32_t& b, uint32_t& c) {
    a -= c; a ^= Rot(c, 4);  c += b;
    b -= a; b ^= Rot(a, 6);  a += c;
    c -= b; c ^= Rot(b, 8);  b += a;
    a -= c; a ^= Rot(c, 16); c += b;
    b -= a; b ^= Rot(a, 19); a += c;
    c -= b; c ^= Rot(b, 4);  b += a;
}

void Final(uint32_t& a, uint32_t& b, uint32_t& c) {
    c ^= b; c -= Rot(b, 14);
    a ^= c; a -= Rot(c, 11);
    b ^= a; b -= Rot(a, 25);
    c ^= b; c -= Rot(b, 16);
    a ^= c; a -= Rot(c, 4);
    b ^= a; b -= Rot(a, 14);
    c ^= b; c -= Rot(b, 24);
}

uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// The shared body: the reference has three copies of each loop for 4-, 2- and 1-byte aligned
// input, all of which add the same little-endian words.
bool Body(const uint8_t* k, uint64_t length, uint32_t& a, uint32_t& b, uint32_t& c) {
    while (length > 12) {
        a += Read32(k);
        b += Read32(k + 4);
        c += Read32(k + 8);
        Mix(a, b, c);
        length -= 12;
        k += 12;
    }
    switch (length) {
        case 12: c += uint32_t(k[11]) << 24; [[fallthrough]];
        case 11: c += uint32_t(k[10]) << 16; [[fallthrough]];
        case 10: c += uint32_t(k[9]) << 8; [[fallthrough]];
        case 9: c += k[8]; [[fallthrough]];
        case 8: b += uint32_t(k[7]) << 24; [[fallthrough]];
        case 7: b += uint32_t(k[6]) << 16; [[fallthrough]];
        case 6: b += uint32_t(k[5]) << 8; [[fallthrough]];
        case 5: b += k[4]; [[fallthrough]];
        case 4: a += uint32_t(k[3]) << 24; [[fallthrough]];
        case 3: a += uint32_t(k[2]) << 16; [[fallthrough]];
        case 2: a += uint32_t(k[1]) << 8; [[fallthrough]];
        case 1: a += k[0]; break;
        case 0: return false;
    }
    Final(a, b, c);
    return true;
}

}

// ref: FUN_14215e000
uint32_t HashLittle(const void* data, uint64_t length, uint32_t initval) {
    uint32_t a = 0xDEADBEEF + static_cast<uint32_t>(length) + initval;
    uint32_t b = a;
    uint32_t c = a;
    Body(static_cast<const uint8_t*>(data), length, a, b, c);
    return c;
}

// ref: FUN_14215da80
void HashLittle2(const void* data, uint64_t length, uint32_t* pc, uint32_t* pb) {
    uint32_t a = 0xDEADBEEF + static_cast<uint32_t>(length) + *pc;
    uint32_t b = a;
    uint32_t c = a + *pb;
    Body(static_cast<const uint8_t*>(data), length, a, b, c);
    *pc = c;
    *pb = b;
}

}
