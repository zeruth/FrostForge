#ifndef UI_C_SIMPLE_STATUS_BAR_HPP
#define UI_C_SIMPLE_STATUS_BAR_HPP

// Engine/Source/Frame/CSimpleStatusBar ("StatusBar", 0x318 bytes; vtable 0x14297fa88,
// CLayoutFrame vtable 0x14297fdb8 at +0x18, CAnimatableObject vtable 0x14297feb0 at +0x1d8): a
// value between a minimum and a maximum, shown by a bar texture anchored to the part of the
// frame the value fills.

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

class CSimpleTexture;
struct TEXTURESOURCE;

class CSimpleStatusBar : public CSimpleFrame {
    public:
    // m_barFlags.
    enum : uint32_t {
        BAR_DIRTY = 0x1,                // the bar texture needs placing
        BAR_RANGE_SET = 0x2,
        BAR_VALUE_SET = 0x4,
        BAR_ROTATES_TEXTURE = 0x8,
        BAR_FILL_STYLE = 0x30,          // FILLSTYLE_*
        BAR_VERTICAL = 0x40,
    };
    // The fill styles (the s_fillStyles names).
    enum : uint8_t {
        FILLSTYLE_STANDARD = 0,
        FILLSTYLE_STANDARD_NO_RANGE_FILL = 1,
        FILLSTYLE_CENTER = 2,
        FILLSTYLE_REVERSE = 3,
    };

    uint32_t m_barFlags;            // 0x300
    float m_minValue;               // 0x304
    float m_maxValue;               // 0x308
    float m_value;                  // 0x30c
    CSimpleTexture* m_barTexture;   // 0x310

    // ref: FUN_1424c2cf0
    explicit CSimpleStatusBar(CSimpleFrame* parent);
    // ref: FUN_1424c2d60
    ~CSimpleStatusBar() override;
    // Status bars come from their own pool (0x1447aef00).
    static void* operator new(size_t size);
    static void operator delete(void* bar);

    // This class's type id (DAT_14309ff04).
    static int32_t GetObjectType();
    // ref: FUN_1424c2df0
    static void CreateScriptMetaTables();
    // ref: FUN_1424c2e40
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d5e53c, DAT_142d5e540).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void RemoveRegion(CSimpleRegion* region) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;

    // The value, clamped to the range (slot 0x318).
    // ref: FUN_1424c3d90
    virtual void SetValue(float value);
    // The bar texture's vertex colour (slot 0x320).
    // ref: FUN_1424c3d50
    virtual void SetStatusBarColor(const uint32_t* rgb, const uint8_t* alpha);
    // How much of the bar is filled, 0..1 (slot 0x328).
    // ref: FUN_1424c2e90
    virtual float GetFillAmount();

    // ref: FUN_1424c3bd0
    void SetMinMaxValues(float minValue, float maxValue);
    // ref: FUN_1424c38e0
    bool SetStatusBarTexture(const TEXTURESOURCE* source);
    // A solid colour for the bar, made when there is no bar texture yet.
    // ref: FUN_1424c3af0
    bool SetColorFill(const uint8_t* rgb, const uint8_t* alpha);
    // ref: FUN_1424c3b90
    void SetFillStyle(uint8_t style);
    // ref: FUN_1424c3cb0
    void SetOrientation(int32_t orientation);
    // ref: FUN_1424c3cd0
    void SetRotatesTexture(bool rotates);
    // ref: FUN_1424c3d70
    void SetStatusBarDesaturation(float desaturation);
    // The bar texture's vertex colour, opaque white without one.
    // ref: FUN_1424c2f30
    uint32_t GetStatusBarColor();
    // ref: FUN_1424c2f80
    float GetStatusBarDesaturation();
    uint8_t GetFillStyle() const {
        return static_cast<uint8_t>((this->m_barFlags & BAR_FILL_STYLE) >> 4);
    }
};
static_assert(sizeof(CSimpleStatusBar) == 0x318, "CSimpleStatusBar is 0x318 bytes");

void CSimpleStatusBar_FillScriptMethods(lua_State* L);

#endif
