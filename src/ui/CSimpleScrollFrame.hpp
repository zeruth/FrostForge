#ifndef UI_C_SIMPLE_SCROLL_FRAME_HPP
#define UI_C_SIMPLE_SCROLL_FRAME_HPP

// Engine/Source/Frame/CSimpleScrollFrame ("ScrollFrame", 0x320 bytes; vtable 0x14297ee78,
// CLayoutFrame vtable 0x14297f190 at +0x18, CAnimatableObject vtable 0x14297f288 at +0x1d8): a
// frame that shows a window onto one scroll child, anchored at the scroll offsets and clipped
// to the frame. The scroll ranges are how far the child overhangs the frame.

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

class CSimpleScrollFrame : public CSimpleFrame {
    public:
    bool m_updateScrollChild;           // 0x300 the ranges are recomputed on the next update
    uint8_t m_pad301[7];
    CSimpleFrame* m_scrollChild;        // 0x308
    float m_horizontalRange;            // 0x310
    float m_verticalRange;              // 0x314
    float m_horizontalScroll;           // 0x318
    float m_verticalScroll;             // 0x31c

    // ref: FUN_1424c0c80
    explicit CSimpleScrollFrame(CSimpleFrame* parent);
    // ref: FUN_1424c0cf0
    ~CSimpleScrollFrame() override;
    // Scroll frames come from their own pool (0x1447aee78, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id (DAT_14309fef4).
    static int32_t GetObjectType();
    // ref: FUN_1424c0d80
    static void CreateScriptMetaTables();
    // ref: FUN_1424c0dd0
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d5e510, DAT_142d5e514).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    bool IsChildShown(CScriptRegion* child) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void OnSizeChanged(float width, float height) override;
    void RemoveChild(CSimpleFrame* child) override;

    // ref: FUN_1424c1560
    void SetScrollChild(CSimpleFrame* child);
    // ref: FUN_1424c1490
    void SetHorizontalScroll(float offset);
    // ref: FUN_1424c16c0
    void SetVerticalScroll(float offset);
    // ref: FUN_1424c17f0
    void UpdateScrollChildRect();

    private:
    // The child's anchor at the scroll offsets.
    void AnchorScrollChild();
    // The ranges from the frame's and the child's rects; OnScrollRangeChanged when they moved.
    void UpdateScrollRanges();
};
static_assert(sizeof(CSimpleScrollFrame) == 0x320, "CSimpleScrollFrame is 0x320 bytes");

void CSimpleScrollFrame_FillScriptMethods(lua_State* L);

#endif
