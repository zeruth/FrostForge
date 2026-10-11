#ifndef GX_GX_RENDER_TARGETS_HPP
#define GX_GX_RENDER_TARGETS_HPP

// The set of targets a pass draws into (0x68 bytes): the depth target at index 0, up to four
// colour targets after it, each as its view and its texture.

#include <cstdint>

namespace gxDx12Backend {
struct TargetView;
struct Tex;
}

class GxRenderContext;

// DAT_14348b658: the device's graphics context while it is up.
extern GxRenderContext* g_gxRenderContext;

// A texture's render-target view, through the device (FUN_14228c430).
gxDx12Backend::TargetView* GxTexTargetView(gxDx12Backend::Tex* tex);

struct GXRENDERTARGETS {
    uint32_t m_ids[5];              // 0x00: what each view is (compared to spot a change)
    uint16_t m_count;               // 0x14: colour targets
    uint16_t m_mask;                // 0x16: bits 0-3 the colour targets, 4 depth, 5 stencil
    gxDx12Backend::TargetView* m_views[5];  // 0x18
    gxDx12Backend::Tex* m_texs[5];  // 0x40
};
static_assert(sizeof(GXRENDERTARGETS) == 0x68, "GXRENDERTARGETS is 0x68 bytes");

// A set to bind after clearing what `m_mask` asks of it (0x58 bytes): bits 0-3 the colour
// targets, each to its own colour, bits 4-5 the depth plane and stencil.
struct GXRENDERTARGETSCLEAR {
    GXRENDERTARGETS* m_targets;     // 0x00
    uint32_t m_mask;                // 0x08: masked again by the set's own
    float m_colors[4][4];           // 0x0c
    float m_depth;                  // 0x4c
    uint8_t m_stencil;              // 0x50
    uint8_t m_pad51[7];
};
static_assert(sizeof(GXRENDERTARGETSCLEAR) == 0x58, "GXRENDERTARGETSCLEAR is 0x58 bytes");

// Empties a set (FUN_14228dec0).
void GxRenderTargetsClear(GXRENDERTARGETS* targets);

// Copies `targets` into `cached`, returning whether nothing changed (FUN_14228dbf0).
bool GxRenderTargetsUpdate(GXRENDERTARGETS* cached, const GXRENDERTARGETS* targets);

// The size of the first colour target at its view's mip, else the depth target's
// (FUN_14228ddb0).
int32_t* GxRenderTargetsSize(const GXRENDERTARGETS* targets, int32_t* size);

// Puts a target into a set, updating the mask (FUN_14228da90), and counts the set's targets
// from its mask (FUN_14228dd30).
void GxRenderTargetsAttach(GXRENDERTARGETS* targets, uint8_t index, gxDx12Backend::Tex* tex);
bool GxRenderTargetsCount(GXRENDERTARGETS* targets);

// Puts one target into a set (FUN_14228de80).
void GxRenderTargetsSet(GXRENDERTARGETS* targets, uint32_t index, gxDx12Backend::Tex* tex);

// Writes the target size constants the shaders read: size, then reciprocals (FUN_14228e2d0).
void GxRenderContextSetTargetConstants(GxRenderContext* context, const GXRENDERTARGETS* targets);

// 0xAARRGGBB to RGBA floats (FUN_141344390).
float* GxColorToFloat4(float* out, uint32_t color);

#endif
