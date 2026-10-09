#include "blz/Murmur.hpp"
#include <cstring>

namespace blz {

namespace {

uint32_t Rotl(uint32_t v, int c) {
    return v << c | v >> (32 - c);
}

}

// ref: FUN_142363e10
uint32_t murmur3_32(const void* data, uint64_t length, uint32_t seed) {
    auto bytes = static_cast<const uint8_t*>(data);
    uint64_t blocks = length >> 2;
    uint32_t h = seed;
    for (uint64_t i = 0; i < blocks; i++) {
        uint32_t k;
        std::memcpy(&k, bytes + i * 4, 4);
        k *= 0xCC9E2D51u;
        k = Rotl(k, 15);
        k *= 0x1B873593u;
        h ^= k;
        h = Rotl(h, 13) * 5 + 0xE6546B64u;
    }
    const uint8_t* tail = bytes + blocks * 4;
    uint32_t k = 0;
    switch (length & 3) {
    case 3:
        k ^= uint32_t(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k ^= uint32_t(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k ^= tail[0];
        k *= 0xCC9E2D51u;
        k = Rotl(k, 15);
        k *= 0x1B873593u;
        h ^= k;
        break;
    default:
        break;
    }
    h ^= static_cast<uint32_t>(length);
    h = (h >> 16 ^ h) * 0x85EBCA6Bu;
    h = (h >> 13 ^ h) * 0xC2B2AE35u;
    return h >> 16 ^ h;
}

namespace {

const uint64_t C1 = 0x87C37B91114253D5ull;  // DAT_142d4baf0
const uint64_t C2 = 0x4CF5AD432745937Full;  // DAT_142d4baf8

uint64_t Rotl64(uint64_t v, int c) {
    return v << c | v >> (64 - c);
}

// The tail: the last length & 15 bytes.
// ref: FUN_142363b60
void Murmur128Tail(uint64_t* h1, uint64_t* h2, const uint8_t* tail, uint32_t length) {
    uint64_t k1 = 0;
    uint64_t k2 = 0;
    switch (length & 15) {
        case 15: k2 = uint64_t(tail[14]) << 48; [[fallthrough]];
        case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
        case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
        case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
        case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
        case 10: k2 ^= uint64_t(tail[9]) << 8; [[fallthrough]];
        case 9:
            k2 = (tail[8] ^ k2) * C2;
            *h2 ^= Rotl64(k2, 33) * C1;
            [[fallthrough]];
        case 8: k1 = uint64_t(tail[7]) << 56; [[fallthrough]];
        case 7: k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
        case 6: k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
        case 5: k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
        case 4: k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
        case 3: k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= uint64_t(tail[1]) << 8; [[fallthrough]];
        case 1:
            k1 = (tail[0] ^ k1) * C1;
            *h1 ^= Rotl64(k1, 31) * C2;
            break;
        default:
            break;
    }
}

}

// ref: FUN_142363ca0
void murmur3_128(uint64_t out[2], const void* data, uint64_t length, uint32_t seed) {
    uint64_t h1 = seed;
    uint64_t h2 = seed;
    auto blocks = static_cast<const uint64_t*>(data);
    uint64_t count = length >> 4;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t k1;
        uint64_t k2;
        std::memcpy(&k1, &blocks[i * 2], 8);
        std::memcpy(&k2, &blocks[i * 2 + 1], 8);
        h1 ^= Rotl64(k1 * C1, 31) * C2;
        h1 = (Rotl64(h1, 27) + h2) * 5 + 0x52DCE729;
        h2 ^= Rotl64(k2 * C2, 33) * C1;
        h2 = (Rotl64(h2, 31) + h1) * 5 + 0x38495AB5;
    }
    Murmur128Tail(&h1, &h2, reinterpret_cast<const uint8_t*>(blocks + count * 2), static_cast<uint32_t>(length));
    h1 ^= length;
    h2 ^= length;
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;
    out[0] = h1;
    out[1] = h2;
}

}
