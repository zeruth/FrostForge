#ifndef WORLD_MODEL_FOG_HPP
#define WORLD_MODEL_FOG_HPP

// The fog a scene draws with (0xa0, initialised by FUN_1402394a0): the colour at +0, the near
// and far distances at +4 and +8, the colour again at +0x30..+0x38. A world scene keeps eight
// (one per view slot, the constructor inlines the initialiser for them) and one more of its
// own; a model frame keeps one and hands it to its scene for each frame it draws.

#include <cstdint>

struct MODELFOG {
    uint32_t m_words[40];

    // ref: FUN_1402394a0
    MODELFOG();

    uint32_t GetColor() const {
        return this->m_words[0];
    }
    float Get(int32_t word) const;
    void Set(int32_t word, float value);
    // The colour in its four slots (FUN_1424bed40's stores).
    void SetColor(uint32_t argb);
};
static_assert(sizeof(MODELFOG) == 0xa0, "MODELFOG is 0xa0 bytes");

#endif
