#ifndef BLZ_RANDOM_HPP
#define BLZ_RANDOM_HPP

// Seed material and the RC4 keystream FrameScript's math.random draws from.

#include <cstddef>
#include <cstdint>

// An entropy provider (RDRAND, or the system generator through BCryptGenRandom).
class ENTROPYPROVIDER {
    public:
    virtual ~ENTROPYPROVIDER() {}
    // Fills `size` bytes; false when the source failed.
    virtual bool Fill(void* buffer, uint64_t size) = 0;
    virtual void Unknown10() {}
};

// The first provider that can be created, made on first use (0x90 bytes).
struct ENTROPYSOURCE {
    uint8_t m_storage[0x80];       // 0x00 the provider, constructed in place
    ENTROPYPROVIDER* m_provider;   // 0x80
    bool m_created;                // 0x88
    int32_t m_index;               // 0x8C a forced provider, or -1 to try each in order
};

ENTROPYSOURCE* EntropySourceInit(ENTROPYSOURCE* source);
void EntropySourceFill(ENTROPYSOURCE* source, void* buffer, uint64_t size);
void EntropySourceRelease(ENTROPYSOURCE* source);

// RC4 state: the permutation and the two indices (0x102 bytes).
struct ARC4STATE {
    uint8_t m_s[256];
    uint8_t m_i;
    uint8_t m_j;
};

void ARC4Init(ARC4STATE* state, const uint8_t* key, uint64_t keyBytes);
// Advances the keystream by `count` bytes without using them.
void ARC4Skip(ARC4STATE* state, uint64_t count);
// XORs `size` bytes of keystream into `buffer`.
void ARC4Process(ARC4STATE* state, uint8_t* buffer, uint64_t size);

#endif
