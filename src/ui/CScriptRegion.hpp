#ifndef UI_C_SCRIPT_REGION_HPP
#define UI_C_SCRIPT_REGION_HPP

// CScriptRegion (Engine/Source/Frame/CScriptRegion.cpp): a scripted object with a layout frame
// -- every frame, texture and font string ("Region", 0x1d8 bytes; vtable 0x1426c9768 with the
// CLayoutFrame vtable 0x1426c99e8 at +0x18). It owns the mouse input settings and the region
// half of XML loading (size, anchors, parentKey, mouse flags).

#include <cstdint>

#include "blz/String.hpp"
#include "framescript/FrameScript.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CScriptObject.hpp"

class CSimpleFrame;
class CStatus;
struct MOUSEEVENT;
struct XMLOBJECT;

// The input event kinds a region can be registered for (bits of m_enabledInput).
enum : int32_t {
    INPUT_EVENT_0 = 0,
    INPUT_EVENT_1 = 1,
    INPUT_EVENT_CLICK = 2,
    INPUT_EVENT_MOTION = 3,
    INPUT_EVENT_WHEEL = 4,
    INPUT_EVENT_5 = 5,
    INPUT_EVENT_6 = 6,
};

class CScriptRegion : public CScriptObject, public CLayoutFrame {
    public:
    // The input flags (m_inputFlags).
    enum : uint8_t {
        REGION_MOUSE_MOTION = 0x1,
        REGION_MOUSE_CLICK = 0x2,
        REGION_PROPAGATE_MOUSE = 0x4,
        REGION_CLICK_THROUGH = 0x8,
    };

    // An optional parent as SetParent receives it: {frame, set}.
    struct PARENTARG {
        CSimpleFrame* m_frame;
        bool m_set;
    };

    CSimpleFrame* m_parent;         // 0x1c0
    uint16_t m_enabledInput;        // 0x1c8 one bit per input event kind
    uint8_t m_inputFlags;           // 0x1ca
    SCRIPTLOCATION m_location;      // 0x1cc where the XML defined it
    uint32_t m_passThroughButtons;  // 0x1d4 mouse buttons that pass through

    CScriptRegion();
    ~CScriptRegion() override;

    // FrameScript_Object and CScriptObject.
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    void SetSourceLocation(const SCRIPTLOCATION* location) override;
    void PreLoadXML(XMLOBJECT* node, CStatus* status) override;
    void OnProtected() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    const SCRIPTLOCATION* GetScriptLocation() override;
    void GetSourceLocation(blz::string_header* text) override;
    CScriptObject* GetParent() override;

    // The region's own slots (0xa0..0x278).
    // The element's own settings; `intrinsic` names the intrinsic type it was created as.
    virtual void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic);
    virtual void LoadXMLScripts(XMLOBJECT* node, CStatus* status, int32_t own);
    virtual void PostLoadXML(XMLOBJECT* node, CStatus* status);
    virtual void SetParentArg(PARENTARG* parent);
    virtual void Hide();
    virtual void Show();
    virtual void SetShown(bool shown);
    virtual void SetAlpha(uint8_t alpha) = 0;
    virtual uint8_t GetAlpha() = 0;
    virtual uint8_t GetEffectiveAlpha();
    virtual bool IsDragging();
    // The cursor moved; whether it is over the region.
    virtual bool OnMouseMove(MOUSEEVENT* event);
    // The key that orders a draw layer of the region among everything drawn.
    virtual uint64_t GetDrawOrderKey(int32_t layer, int32_t subLevel);
    virtual uint64_t GetDrawOrder();
    // Whether it draws inside a frame that batches its own drawing.
    virtual bool IsBatched();
    virtual bool HasCharHandler();
    virtual bool Unknown120();
    virtual bool HasKeyHandler(const void* event);
    virtual bool HasPadButtonHandler(void* event);
    virtual bool HasPadStickHandler(void* event);
    virtual bool HasWheelHandler();
    virtual bool OnChar(int32_t key);
    virtual bool Unknown150(void* event);
    virtual bool OnKeyDown(void* event);
    virtual bool OnKeyUp(void* event);
    virtual bool Unknown168(void* event);
    virtual bool OnMouseDown(MOUSEEVENT* event, const char* button);
    virtual bool OnMouseUp(MOUSEEVENT* event, const char* button);
    virtual bool OnMouseWheel(MOUSEEVENT* event);
    virtual bool OnPadButtonDown(void* event);
    virtual bool OnPadButtonUp(void* event);
    virtual bool OnPadStick(void* event);
    virtual CScriptRegion* GetMouseTarget(int32_t type, MOUSEEVENT* event, bool* handled, bool force);
    // 0 a miss, 1 a hit, 2 a hit on a hyperlink.
    virtual int32_t HitTest(int32_t type, MOUSEEVENT* event, bool force);
    virtual bool IsMouseMotionEnabled();
    virtual bool IsMouseClickEnabled();
    virtual void EnableMouseWheel(bool enable);
    virtual bool IsMouseWheelEnabled();
    virtual bool IsMouseEnabled();
    virtual void EnableMouse(bool enable);
    virtual void SetMouseMotionEnabled(bool enable);
    // Whether hyperlinks in it are handled by its parent, and whether it handles them.
    virtual bool GetHyperlinksPropagated();
    virtual bool GetHyperlinksEnabled();
    virtual void QueueOnShow();
    virtual void QueueOnHide();
    virtual void OnEnter(bool motion);
    virtual void OnLeave(bool motion);
    virtual void OnMouseOverChanged(bool over);
    virtual void OnMouseFocusLost(bool clear, bool quiet);
    virtual void OnDragStart(MOUSEEVENT* event);
    virtual void OnDragStop(MOUSEEVENT* event);
    virtual void OnReceiveDrag(MOUSEEVENT* event);
    virtual void OnLayoutVersionChanged();
    virtual const float* GetHitRectInsets();
    virtual bool IsMouseOver(const float* point);
    virtual CRect* TransformRect(CRect* out, const CRect* rect);
    // Whether it sits inside a scroll frame's scroll child.
    virtual bool IsInScrollFrame();
    virtual bool IsOnScreen();
    virtual void OnScriptHandlerLoaded(XMLOBJECT* handler);
    virtual void RunOnLoad();

    // CLayoutFrame.
    void* GetLayoutParent() override;
    CScriptRegion* GetScriptObject() override;
    CScriptRegion* GetScriptRegion() override;
    CLayoutFrame* GetRelativeByName(const char* name, CLayoutFrame* self) override;

    // This class's type id (0x142e97b04).
    static int32_t GetObjectType();

    CRect* GetHitRect(CRect* rect);
    bool CanQueryRect();
    bool IsAccessibleToScripts();
    void EnableInputEvent(int32_t type);
    void SetInputEvent(int32_t type, bool enable);
    void SetMouseClickEnabled(bool enable);
    void SetPassThroughButtons(const uint32_t* buttons, uint64_t count);
    void SetInputFlag(uint8_t flag, bool set);
    bool HasInputFlags(uint8_t flags);
    void UpdateClickRegistration();
    void UpdateMotionRegistration();
    void RegisterEnabledInput();
    void UnregisterEnabledInput(bool quiet);
    bool IsMouseOverOffset(float top, float bottom, float left, float right);
    bool IsInMotionList();
    blz::string* GetSourceLocationString(blz::string* text);
};
static_assert(sizeof(CScriptRegion) == 0x1d8, "CScriptRegion is 0x1d8 bytes");

// Loads one <Anchor> (FUN_14060d280) or <StartAnchor>/<EndAnchor> (FUN_14060d520) element.
void CScriptRegion_LoadAnchor(XMLOBJECT* anchor, CScriptRegion* region, CLayoutFrame* layout, CStatus* status);
void CScriptRegion_LoadCenteredAnchor(XMLOBJECT* anchor, CScriptRegion* region, CLayoutFrame* layout, CStatus* status);
// The names of mouse buttons ("LeftButton", ...) and their bits.
const char* MouseButtonToString(int32_t button);
uint32_t StringToMouseButton(const char* name);

#endif
