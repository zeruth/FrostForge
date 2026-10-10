#ifndef WORLD_WORLD_FADE_HPP
#define WORLD_WORLD_FADE_HPP

// The scene's fade list at +0x58 (0x178 bytes): an embedded vector of eight InnerFadeInfo
// (0x28 each) and the state the animate pass updates it from (FUN_141370460, when +0x170 is
// set). Its source file is not known.

#include <cstddef>
#include <cstdint>

struct InnerFadeInfo {
    uint8_t m_data[0x28];
};

class CWorldFade {
    public:
    InnerFadeInfo* m_data;          // 0x000 the inline storage, until it grows
    uint64_t m_size;                // 0x008
    uint64_t m_capacity;            // 0x010 bit 63: the inline storage
    InnerFadeInfo m_inline[8];      // 0x018
    uint64_t m_158;                 // 0x158
    uint64_t m_160;                 // 0x160
    uint64_t m_168;                 // 0x168
    uint8_t m_active;               // 0x170
    uint8_t m_pad171[3];
    uint32_t m_174;                 // 0x174

    // ref: FUN_141370120
    CWorldFade();
    // ref: FUN_141370160
    ~CWorldFade();
};
static_assert(offsetof(CWorldFade, m_158) == 0x158, "CWorldFade layout");
static_assert(sizeof(CWorldFade) == 0x178, "CWorldFade is 0x178 bytes");

#endif
