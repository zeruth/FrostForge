#ifndef MODEL2_M2_RIBBON_HPP
#define MODEL2_M2_RIBBON_HPP

// CRibbonEmitter (Engine/Source/Model2 ribbons, 0x1d8): the trail an M2Ribbon leaves behind its
// bone, kept as a ring of edges. Only what the model code reads is named so far; the emitter's
// functions (0x141296530..0x1412980xx) are ported with M2Ribbon.cpp.

#include <cstddef>
#include <cstdint>

#include "model2/M2Data.hpp"

struct C44Matrix;

class CRibbonEmitter {
    public:
    uint8_t m_00[0xb4];
    CAaBox m_bounds;                    // 0xb4 around the edges
    uint8_t m_cc[0x120 - 0xcc];
    void* m_texture2;                   // 0x120 a second texture: three layers instead of one
    uint8_t m_128[0x1cc - 0x128];
    float m_heightAbove;                // 0x1cc
    float m_heightBelow;                // 0x1d0
    uint8_t m_1d4[4];
};
static_assert(offsetof(CRibbonEmitter, m_heightAbove) == 0x1cc, "CRibbonEmitter heights at 0x1cc");
static_assert(sizeof(CRibbonEmitter) == 0x1d8, "CRibbonEmitter is 0x1d8 bytes");

class CTexture;

// A ribbon made in place (FUN_141296530) and set up with its material, colour, textures, blend
// and texture types (FUN_1412966c0); its edges cleared (FUN_141296cf0); its quality from
// RibbonQuality (FUN_141296f20, the id kept at 0x1a0); a texture layer (FUN_141297050); on or off
// (FUN_1412970f0); a colour override (FUN_141296f00).
CRibbonEmitter* RibbonEmitterConstruct(void* memory);
void RibbonEmitterInitialize(CRibbonEmitter* ribbon, uint32_t material, const uint32_t* color, CTexture** textures,
    const uint32_t* blend, const uint32_t* types);
void RibbonEmitterReset(CRibbonEmitter* ribbon);
void RibbonEmitterSetQuality(CRibbonEmitter* ribbon, uint32_t quality);
uint16_t RibbonEmitterGetQuality(const CRibbonEmitter* ribbon);
void RibbonEmitterSetTexture(CRibbonEmitter* ribbon, uint32_t layer, CTexture* texture);
void RibbonEmitterSetEnabled(CRibbonEmitter* ribbon, bool enabled);
void RibbonEmitterSetColorOverride(CRibbonEmitter* ribbon, float r, float g, float b);
// The matrix the ribbon's edges are kept relative to, null for world space (FUN_1412966b0).
void RibbonEmitterSetParentMatrix(CRibbonEmitter* ribbon, const C44Matrix* matrix);
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
void RibbonEmitterUpdate(CRibbonEmitter* ribbon, float delta, bool hidden);
// ref: FUN_141296640
void RibbonEmitterDelete(CRibbonEmitter* ribbon);

// Whether a ribbon has nothing to draw: emitting and with no edges past its first
// (FUN_141296840).
bool RibbonEmitterIsIdle(const CRibbonEmitter* ribbon);

#endif
