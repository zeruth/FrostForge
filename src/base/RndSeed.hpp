#ifndef BASE_RND_SEED_HPP
#define BASE_RND_SEED_HPP

// The engine's random stream (NTempest's CRndSeed): a running sum and four byte cursors over a
// table of 0xf8 bytes (DAT_142734470), read as dwords at those byte offsets. Each step moves the
// cursors and adds the rotated words they land on to the sum.

#include <cstdint>

struct CRndSeed {
    uint32_t m_sum;                 // 0x00
    uint32_t m_cursors;             // 0x04
};
static_assert(sizeof(CRndSeed) == 8, "CRndSeed is 8 bytes");

extern const uint8_t g_rndSeedTable[0xf8];

// Seeded from a value: the value, and a mix of its residues for the cursors (FUN_14133cae0).
void RndSeedInit(CRndSeed* seed, uint32_t value);
// The next sum (FUN_1403068a0).
uint32_t RndSeedNext(CRndSeed* seed);
// The next sum as a float in [0, 1).
float RndSeedUnit(CRndSeed* seed);
// The next sum as a float in [-1, 1]: [1, 2) taken from 2, or 2 taken from it, by the sum's
// sign (FUN_14125abd0 is this on a generator's stream).
float RndSeedSigned(CRndSeed* seed);
// A sum's mantissa as a float in [1, 2).
float RndSeedMantissa(uint32_t sum);

#endif
