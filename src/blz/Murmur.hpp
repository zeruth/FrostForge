#ifndef BLZ_MURMUR_HPP
#define BLZ_MURMUR_HPP

#include <cstdint>

namespace blz {

// MurmurHash3_x86_32.
uint32_t murmur3_32(const void* data, uint64_t length, uint32_t seed);

// MurmurHash3_x64_128; the string hash of blz's chained tables (the low word picks the bucket).
void murmur3_128(uint64_t out[2], const void* data, uint64_t length, uint32_t seed);

// MurmurHash3's 64-bit finalizer, which blz's integer keys hash with.
inline uint64_t fmix64(uint64_t k) {
    k = (k >> 33 ^ k) * 0xFF51AFD7ED558CCDull;
    k = (k >> 33 ^ k) * 0xC4CEB9FE1A85EC53ull;
    return k >> 33 ^ k;
}

}

#endif
