#ifndef UI_C_SIMPLE_SLIDER_HPP
#define UI_C_SIMPLE_SLIDER_HPP

// Engine/Source/Frame/CSimpleSlider ("Slider", 0x320 bytes; vtable 0x14297f570, CLayoutFrame
// vtable 0x14297f888 at +0x18, CAnimatableObject vtable 0x14297f980 at +0x1d8): a value between
// a minimum and a maximum, snapped to a step, shown by a thumb texture placed along the frame
// and dragged (or paged with clicks beside the thumb) with the mouse.

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

class CSimpleTexture;
struct TEXTURESOURCE;

class CSimpleSlider : public CSimpleFrame {
    public:
    // m_sliderFlags: the low byte is the steps per page, then the orientation and the state.
    enum : uint32_t {
        SLIDER_STEPS_PER_PAGE = 0xff,
        SLIDER_ORIENTATION = 0x300,
        SLIDER_THUMB_DIRTY = 0x400,
        SLIDER_RANGE_SET = 0x800,
        SLIDER_VALUE_SET = 0x1000,
        SLIDER_DRAGGING = 0x2000,
        SLIDER_OBEY_STEP_ON_DRAG = 0x4000,
    };

    float m_minValue;               // 0x300
    float m_range;                  // 0x304 max - min
    float m_value;                  // 0x308
    float m_valueStep;              // 0x30c
    uint32_t m_sliderFlags;         // 0x310
    uint32_t m_pad314;
    CSimpleTexture* m_thumbTexture; // 0x318

    // ref: FUN_1424c1b40
    explicit CSimpleSlider(CSimpleFrame* parent);
    // ref: FUN_1424c1be0
    ~CSimpleSlider() override;
    // Sliders come from their own pool (0x1447aeec0, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* slider);

    // This class's type id (DAT_14309fefc).
    static int32_t GetObjectType();
    // ref: FUN_1424c1c50
    static void CreateScriptMetaTables();
    // ref: FUN_1424c1ca0
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d5e518, DAT_142d5e51c).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    bool OnMouseMove(MOUSEEVENT* event) override;
    bool OnMouseDown(MOUSEEVENT* event, const char* button) override;
    bool OnMouseUp(MOUSEEVENT* event, const char* button) override;
    void QueueOnHide() override;
    void RemoveRegion(CSimpleRegion* region) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;

    int32_t GetOrientation() const;
    // ref: FUN_1424c2870
    void SetOrientation(int32_t orientation);
    // ref: FUN_1424c27a0
    void SetMinMaxValues(float minValue, float maxValue);
    // ref: FUN_1424c2a90
    void SetValue(float value, bool userInput);
    // ref: FUN_1424c2b90
    void SetValueStep(float step);
    // ref: FUN_1424c28c0
    bool SetThumbTexture(const TEXTURESOURCE* source, uint32_t drawLayer);
    // ref: FUN_1424c2020
    bool IsDraggingThumb() const;
    // The value under the mouse while dragging (FUN_1424c1d30).
    void UpdateFromMouse(MOUSEEVENT* event);

    private:
    // A value snapped to the step and clamped to the range.
    float StepValue(float value, bool snap) const;
};
static_assert(sizeof(CSimpleSlider) == 0x320, "CSimpleSlider is 0x320 bytes");

void CSimpleSlider_FillScriptMethods(lua_State* L);

#endif
