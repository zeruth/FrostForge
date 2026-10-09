#ifndef UI_C_SIMPLE_REGION_HPP
#define UI_C_SIMPLE_REGION_HPP

// CSimpleRegion (Engine/Source/Frame/CSimpleRegion.cpp): the drawable part of a frame -- the
// base of textures, font strings and lines ("Region", 0x218 bytes; vtable 0x1426cba68 with
// the CLayoutFrame vtable 0x1426cbd40 at +0x18 and the CAnimatableObject vtable 0x1426cbe38 at
// +0x1d8). A region sits in one draw layer of its frame and carries a colour (or a gradient),
// an alpha and a scale.

#include <cstdint>

#include "storm/list/TSLink.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/UITypes.hpp"

class CRenderBatch;
class CSimpleFrame;

// A four-corner gradient (CDataAllocator 0x142e99d00, 0x1c): an alpha and a colour per corner.
struct CGradient {
    uint32_t m_count;               // 0x00 4
    uint8_t m_alpha[4];             // 0x04
    uint32_t m_capacity;            // 0x08 4
    uint32_t m_colors[4];           // 0x0C
};

class CSimpleRegion : public CScriptRegion, public CAnimatableObject {
    public:
    // m_regionFlags.
    enum : uint8_t {
        REGIONFLAG_GRADIENT = 0x1,          // m_gradient holds the colour
        REGIONFLAG_IGNORE_PARENT_ALPHA = 0x2,
        REGIONFLAG_IGNORE_PARENT_SCALE = 0x4,
        REGIONFLAG_ITERATING = 0x8,         // the frame is walking past it: deletes wait
    };

    // A colour, or the gradient that replaced it.
    union {
        uint32_t m_color;           // 0x1e8 ARGB; the alpha byte holds the effective alpha
        CGradient* m_gradient;
    };
    TSLink<CSimpleRegion> m_regionLink;     // 0x1f0 the frame's regions
    TSLink<CSimpleRegion> m_drawLink;       // 0x200 the frame's visible regions
    float m_scale;                  // 0x210
    uint8_t m_alpha;                // 0x214
    uint8_t m_drawLayer;            // 0x215
    int8_t m_subLevel;              // 0x216 -8..7
    uint8_t m_regionFlags;          // 0x217

    // The number of regions alive (0x142e99ce0).
    static int64_t s_count;

    // ref: FUN_1406309c0
    CSimpleRegion(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show);
    ~CSimpleRegion() override;

    // FrameScript_Object, CScriptObject and CScriptRegion.
    CAnimatableObject* GetAnimatableObject() override;
    void DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void SetParentArg(PARENTARG* parent) override;
    void Hide() override;
    void Show() override;
    void SetShown(bool shown) override;
    void SetAlpha(uint8_t alpha) override;
    uint8_t GetAlpha() override;

    // The region's own slots (0x280..0x2d0).
    // Sets the colour; the alpha only when `alpha` says so.
    virtual void SetVertexColor(uint32_t rgb, const uint8_t* alpha);
    // Recomputes the alpha the colour carries (the region's alpha times its frame's).
    virtual void UpdateEffectiveAlpha();
    virtual void Unknown290();
    // Adds the region's geometry to a batch.
    virtual void AddToBatch(CRenderBatch* batch) = 0;
    virtual void SetDrawLayer(uint8_t layer, int8_t subLevel);
    virtual void GetDrawLayer(uint32_t* layer, int8_t* subLevel);
    virtual uint64_t GetOwnDrawOrderKey();
    virtual void Unknown2B8();
    virtual void Unknown2C0();
    virtual void Unknown2C8();
    virtual void Unknown2D0();

    // CLayoutFrame.
    bool SetLayoutScale(float scale, bool force) override;
    bool CanBeAnchorFor(CLayoutFrame* frame) override;
    void OnLayoutReady() override;
    void ApplyHide() override;
    void ApplyShow() override;

    // CAnimatableObject.
    void OnAnimStarted(bool alpha, bool transform, bool rectOffset) override;
    void OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset) override;
    void SetAnimAlpha(uint8_t alpha) override;
    CScriptRegion* GetScriptRegion() override;

    // Moves the region to a frame and draw layer, shown or hidden.
    void SetFrame(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show);
    // The colour with the region's own alpha (white for a gradient).
    bool GetVertexColor(uint32_t* argb);
    void SetVertexColor(const uint32_t* argb);
    void SetGradient(int32_t orientation, const uint32_t* minColor, const uint32_t* maxColor);
    void SetHasGradient(bool gradient);
    void SetIgnoreParentScale(bool ignore);
    void SetScale(float scale);
    // Tells the frame its batch must be rebuilt.
    void MarkFrameBatchDirty();

    // Regions deleted while their frame walks its list are deleted afterwards.
    static void DeferDelete(CSimpleRegion* region);
    static void FlushDeferredDeletes();
};
static_assert(sizeof(CSimpleRegion) == 0x218, "CSimpleRegion is 0x218 bytes");

#endif
