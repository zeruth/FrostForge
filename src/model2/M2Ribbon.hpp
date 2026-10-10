#ifndef MODEL2_M2_RIBBON_HPP
#define MODEL2_M2_RIBBON_HPP

// CRibbonEmitter (Engine/Source/Model2 ribbons, 0x1d8): the trail an M2Ribbon leaves behind its
// bone, kept as a ring of edges. Only what the model code reads is named so far; the emitter's
// functions (0x141296530..0x1412980xx) are ported with M2Ribbon.cpp.

#include <cstddef>
#include <cstdint>

#include "model2/M2Data.hpp"

class CRibbonEmitter {
    public:
    uint8_t m_00[0x120];
    void* m_texture2;                   // 0x120 a second texture: three layers instead of one
    uint8_t m_128[0x1cc - 0x128];
    float m_heightAbove;                // 0x1cc
    float m_heightBelow;                // 0x1d0
    uint8_t m_1d4[4];
};
static_assert(offsetof(CRibbonEmitter, m_heightAbove) == 0x1cc, "CRibbonEmitter heights at 0x1cc");
static_assert(sizeof(CRibbonEmitter) == 0x1d8, "CRibbonEmitter is 0x1d8 bytes");

// ref: FUN_141296be0
void RibbonEmitterSetColor(CRibbonEmitter* ribbon, float r, float g, float b);
// ref: FUN_141296bc0
void RibbonEmitterSetAlpha(CRibbonEmitter* ribbon, float alpha);
// ref: FUN_141296cc0
void RibbonEmitterSetVisible(CRibbonEmitter* ribbon, bool visible);
// ref: FUN_1412970b0
void RibbonEmitterSetTextureTransform(CRibbonEmitter* ribbon, uint32_t layer, const C4Quaternion* rotation,
    const C3Vector* scale, const C3Vector* translation);
// ref: FUN_141296df0
void RibbonEmitterSetMatrix(CRibbonEmitter* ribbon, const float* matrix, const void* a3);
// ref: FUN_141297490
void RibbonEmitterUpdate(CRibbonEmitter* ribbon, uint32_t time, bool hidden);

#endif
