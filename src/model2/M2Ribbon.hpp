#ifndef MODEL2_M2_RIBBON_HPP
#define MODEL2_M2_RIBBON_HPP

// CRibbonEmitter (Engine/Source/Model2 ribbons, 0x1d8): the trail an M2Ribbon leaves behind its
// bone, laid down as a GpuStripEmitter strip edge by edge between its height above and below,
// with up to three texture layers scrolled over it.

#include <cstddef>
#include <cstdint>

#include "model2/GpuStripEmitter.hpp"
#include "model2/M2Data.hpp"

struct C44Matrix;
class CTexture;
class GxRenderContext;

class CRibbonEmitter {
    public:
    float m_maxSampleTime;              // 0x000 RibbonQuality's MaxSampleTimeDelta
    float m_angleThreshold;             // 0x004 RibbonQuality's AngleThreshold
    GpuStripEmitter m_strip;            // 0x008
    const C44Matrix* m_drawMatrix;      // 0x0e8 the draw's: the model's attachment matrix,
    const float* m_drawView;            // 0x0f0 the view
    const float* m_drawProjection;      // 0x0f8 and the projection
    uint64_t m_100;                     // 0x100
    C3Vector m_drawCamera;              // 0x108
    uint32_t m_pad114;
    CTexture* m_textures[3];            // 0x118
    float m_uvScale[3][2];              // 0x130 each layer's
    float m_uvOffset[3][2];             // 0x148 each layer's, scrolled
    uint32_t m_color;                   // 0x160 a r g b
    int32_t m_blend;                    // 0x164 the draw's blend, from 0x19c
    float m_uvSpeedU[3];                // 0x168
    float m_uvSpeedV[3];                // 0x174
    uint32_t m_textureTypes[3];         // 0x180
    float m_uvStart[4];                 // 0x18c the random start of layers 1 and 2
    int32_t m_blendMode;                // 0x19c the material's
    uint16_t m_quality;                 // 0x1a0 the RibbonQuality id
    // 1 positioned, 2 colour override, 4 set up, 8 visible
    uint8_t m_flags;                    // 0x1a2
    uint8_t m_pad1a3[5];
    C3Vector m_position;                // 0x1a8
    C3Vector m_direction;               // 0x1b4 up the ribbon
    C3Vector m_colorOverride;           // 0x1c0
    float m_heightAbove;                // 0x1cc
    float m_heightBelow;                // 0x1d0
    uint32_t m_pad1d4;
};
static_assert(offsetof(CRibbonEmitter, m_strip) == 0x8, "CRibbonEmitter strip at 0x8");
static_assert(offsetof(CRibbonEmitter, m_textures) == 0x118, "CRibbonEmitter textures at 0x118");
static_assert(offsetof(CRibbonEmitter, m_color) == 0x160, "CRibbonEmitter colour at 0x160");
static_assert(offsetof(CRibbonEmitter, m_flags) == 0x1a2, "CRibbonEmitter flags at 0x1a2");
static_assert(offsetof(CRibbonEmitter, m_heightAbove) == 0x1cc, "CRibbonEmitter heights at 0x1cc");
static_assert(sizeof(CRibbonEmitter) == 0x1d8, "CRibbonEmitter is 0x1d8 bytes");

// A ribbon made in place (FUN_141296530) and set up with its edges' lifetime, colour, textures,
// blend and texture types (FUN_1412966c0); its edges' gravity (FUN_141296cf0); its quality from
// RibbonQuality (FUN_141296f20, the id kept at 0x1a0); a texture layer (FUN_141297050); on or off
// (FUN_1412970f0); a colour override (FUN_141296f00).
CRibbonEmitter* RibbonEmitterConstruct(void* memory);
void RibbonEmitterInitialize(CRibbonEmitter* ribbon, float lifetime, const uint32_t* color, CTexture** textures,
    const uint32_t* blend, const uint32_t* types);
void RibbonEmitterSetGravity(CRibbonEmitter* ribbon, float gravity);
void RibbonEmitterSetQuality(CRibbonEmitter* ribbon, uint32_t quality);
uint16_t RibbonEmitterGetQuality(const CRibbonEmitter* ribbon);
void RibbonEmitterSetTexture(CRibbonEmitter* ribbon, uint32_t layer, CTexture* texture);
void RibbonEmitterSetEnabled(CRibbonEmitter* ribbon, bool enabled);
void RibbonEmitterSetColorOverride(CRibbonEmitter* ribbon, float r, float g, float b);
// The matrix the ribbon's edges are kept relative to, null for world space (FUN_1412966b0).
void RibbonEmitterSetParentMatrix(CRibbonEmitter* ribbon, const C44Matrix* matrix);
// The colour (FUN_141296be0), alpha (FUN_141296bc0) and whether it is shown (FUN_141296cc0).
void RibbonEmitterSetColor(CRibbonEmitter* ribbon, float r, float g, float b);
void RibbonEmitterSetAlpha(CRibbonEmitter* ribbon, float alpha);
void RibbonEmitterSetVisible(CRibbonEmitter* ribbon, bool visible);
// A texture layer's scale and scroll speed (FUN_1412970b0).
void RibbonEmitterSetTextureTransform(CRibbonEmitter* ribbon, uint32_t layer, const C4Quaternion* rotation,
    const C3Vector* scale, const C3Vector* translation);
// Where the ribbon is and which way is up, through the attachment's inverse when there is one
// (FUN_141296df0).
void RibbonEmitterSetMatrix(CRibbonEmitter* ribbon, const float* matrix, const void* a3);
// The frame: an edge laid down while shown, the strip aged and the layers scrolled
// (FUN_141297490).
void RibbonEmitterUpdate(CRibbonEmitter* ribbon, float delta, bool hidden);
// The ribbon taken down (FUN_141296640).
void RibbonEmitterDelete(CRibbonEmitter* ribbon);

// Whether a ribbon has nothing to draw: set up and with no segment past the dead ones
// (FUN_141296840).
bool RibbonEmitterIsIdle(const CRibbonEmitter* ribbon);
struct C3Vector;
// What a ribbon's draw is handed: the context, the model's attachment matrix and the camera.
struct RIBBONDRAWPARAMS {
    GxRenderContext* m_context;         // 0x00
    const C44Matrix* m_matrix;          // 0x08
    const C3Vector* m_camera;           // 0x10
};
// The ribbon's strip built and drawn through `view` and `projection` (FUN_141296af0).
void RibbonEmitterDraw(CRibbonEmitter* ribbon, const RIBBONDRAWPARAMS* params, const C44Matrix* view,
    const C44Matrix* projection);

#endif
