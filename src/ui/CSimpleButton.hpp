#ifndef UI_C_SIMPLE_BUTTON_HPP
#define UI_C_SIMPLE_BUTTON_HPP

// Engine/Source/Frame/CSimpleButton ("Button", 0x368 bytes; vtable 0x14297c1b8, CLayoutFrame
// vtable 0x14297c500 at +0x18, CAnimatableObject vtable 0x14297c5f8 at +0x1d8): a frame with a
// texture per state (disabled, normal, pushed), a highlight texture, a font string whose font
// object follows the state, and the click scripts (OnClick with PreClick and PostClick around
// it, OnDoubleClick).

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

class CSimpleFont;
class CSimpleFontString;
class CSimpleTexture;
struct TEXTURESOURCE;

class CSimpleButton : public CSimpleFrame {
    public:
    // The button states (the low four bits of m_state).
    enum : int32_t {
        BUTTON_DISABLED = 0,
        BUTTON_NORMAL = 1,
        BUTTON_PUSHED = 2,
    };

    // m_buttonFlags.
    enum : uint8_t {
        BUTTON_IN_CLICK = 0x1,
        BUTTON_STATE_LOCKED = 0x2,
        BUTTON_MOTION_WHILE_DISABLED = 0x4,
    };

    uint32_t m_lastClickTime;               // 0x300 for OnDoubleClick
    uint32_t m_pad304;
    uint64_t m_registeredClicks;            // 0x308 the click kinds that run OnClick
    uint64_t m_registeredMouse;             // 0x310 the mouse buttons handled at all
    uint32_t m_state;                       // 0x318
    uint8_t m_buttonFlags;                  // 0x31c
    uint8_t m_pad31d[3];
    CSimpleFontString* m_text;              // 0x320
    CSimpleFont* m_normalFont;              // 0x328
    CSimpleFont* m_highlightFont;           // 0x330
    CSimpleFont* m_disabledFont;            // 0x338
    float m_pushedTextOffset[2];            // 0x340
    CSimpleTexture* m_stateTextures[3];     // 0x348 disabled, normal, pushed
    CSimpleTexture* m_highlightTexture;     // 0x360

    // ref: FUN_1424aac80
    explicit CSimpleButton(CSimpleFrame* parent);
    // ref: FUN_1424aad80
    ~CSimpleButton() override;
    // Buttons come from their own pool (0x1447aed30, 0x190 per block).
    static void* operator new(size_t size);
    static void operator delete(void* button);

    // This class's type id (DAT_14309fd34).
    static int32_t GetObjectType();
    // ref: FUN_1424aafd0
    static void CreateScriptMetaTables();
    // ref: FUN_1424ab020
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d4e470, DAT_142d4e474).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ---- FrameScript_Object / CScriptRegion / CSimpleFrame overrides -----------------------
    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    bool OnMouseDown(MOUSEEVENT* event, const char* button) override;
    bool OnMouseUp(MOUSEEVENT* event, const char* button) override;
    void QueueOnHide() override;
    void OnEnter(bool motion) override;
    void OnLeave(bool motion) override;
    void OnDragStart(MOUSEEVENT* event) override;
    void RemoveRegion(CSimpleRegion* region) override;
    void LockHighlight(bool lock) override;
    void ScaleBy(float* scale) override;

    // ---- the button's own slots (0x318..0x340) ---------------------------------------------
    // ref: FUN_1424ac140
    virtual void SetEnabled(bool enabled);
    // Runs PreClick, OnClick and PostClick (FUN_1424ab9e0).
    virtual void OnClick(const char* button, bool down, bool fromInput, bool secure, const char* taint);
    // ref: FUN_1424abb10
    virtual void OnDoubleClick(const char* button);
    // ref: FUN_1424ac040
    virtual void SetButtonState(int32_t state, bool locked);
    // ref: FUN_1424ac6b0
    virtual void SetMotionScriptsWhileDisabled(bool enabled);
    // ref: FUN_1424ab110
    virtual bool GetMotionScriptsWhileDisabled();

    // ---- the rest ---------------------------------------------------------------------------
    // The script's Click (FUN_1424aaf40): forbidden frames refuse.
    void Click(const char* button, bool down);
    // The state's texture (FUN_1424ac750): an existing texture is adopted, anything else is
    // set on the state's own texture (made when missing).
    bool SetStateTexture(int32_t state, const TEXTURESOURCE* source);
    // ref: FUN_1424ac420
    bool SetHighlight(const TEXTURESOURCE* source, const uint8_t* blendMode);
    // ref: FUN_1424ac340
    void SetFontString(CSimpleFontString* text);
    // ref: FUN_1424ac8e0
    void SetText(const char* text);
    // The text's font and offset for a state (FUN_1424acaf0).
    void UpdateTextState(int32_t state);
    // ref: FUN_1424ab120 / FUN_1424ab140
    CSimpleTexture* GetNormalTexture();
    CSimpleTexture* GetPushedTexture();
    // ref: FUN_1424ab180
    float GetTextHeight();
    // ref: FUN_1424ab1a0
    float GetTextWidth();
    // The clears (FUN_1424aae50, FUN_1424aae90, FUN_1424aaec0, FUN_1424aaf00).
    void ClearDisabledTexture();
    void ClearHighlightTexture();
    void ClearNormalTexture();
    void ClearPushedTexture();
    // ref: FUN_1424ac000 / FUN_1424ac020
    void RegisterForClicks(const uint64_t* masks, uint64_t count);
    void RegisterForMouse(const uint64_t* masks, uint64_t count);
};
static_assert(sizeof(CSimpleButton) == 0x368, "CSimpleButton is 0x368 bytes");

// A click kind's mask, 0 for an unknown name ("LeftButtonUp", "AnyDown", ..., FUN_1424deaf0).
uint64_t ButtonClickMask(const char* name);
void CSimpleButton_FillScriptMethods(lua_State* L);

#endif
