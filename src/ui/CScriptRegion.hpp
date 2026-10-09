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
    INPUT_EVENT_MOTION = 2,
    INPUT_EVENT_CLICK = 3,
    INPUT_EVENT_WHEEL = 4,
    INPUT_EVENT_5 = 5,
    INPUT_EVENT_6 = 6,
};

class CScriptRegion : public CScriptObject, public CLayoutFrame {
    public:
    // The input flags (m_inputFlags).
    enum : uint8_t {
        REGION_MOUSE_CLICK = 0x1,
        REGION_MOUSE_MOTION = 0x2,
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
    void GetSourceLocation(blz::string_header* text) override;
    CScriptObject* GetParent() override;

    // The region's own slots (0xa0..0x278).
    virtual void LoadXML(XMLOBJECT* node, CStatus* status);
    virtual void LoadXMLScripts(XMLOBJECT* node, CStatus* status, int32_t own);
    virtual void PostLoadXML(XMLOBJECT* node, CStatus* status);
    virtual void SetParentArg(PARENTARG* parent);
    virtual void UnknownC0();
    virtual void UnknownC8();
    virtual void UnknownD0();
    virtual void SetAlpha(uint8_t alpha) = 0;
    virtual uint8_t GetAlpha() = 0;
    virtual uint8_t GetEffectiveAlpha();
    virtual bool UnknownF0();
    virtual bool UnknownF8();
    virtual void* Unknown100();
    virtual uint64_t GetDrawOrder();
    virtual bool HasOwnBatch();
    virtual bool Unknown118();
    virtual bool Unknown120();
    virtual bool Unknown128(void* event);
    virtual bool Unknown130(void* event);
    virtual bool Unknown138(void* event);
    virtual bool Unknown140(void* event);
    virtual bool Unknown148(int32_t key);
    virtual bool Unknown150(void* event);
    virtual bool Unknown158(void* event);
    virtual bool Unknown160(void* event);
    virtual bool Unknown168(void* event);
    virtual void OnMouseDown(MOUSEEVENT* event, const char* button);
    virtual void OnMouseUp(MOUSEEVENT* event, const char* button);
    virtual bool Unknown180(void* event);
    virtual bool Unknown188(void* event);
    virtual bool Unknown190(void* event);
    virtual bool Unknown198(void* event);
    virtual CScriptRegion* GetMouseTarget(int32_t type, MOUSEEVENT* event, bool* handled, bool force);
    virtual bool HitTest(int32_t type, MOUSEEVENT* event, bool force);
    virtual bool IsMouseClickEnabled();
    virtual bool IsMouseMotionEnabled();
    virtual void EnableMouseWheel(bool enable);
    virtual bool IsMouseWheelEnabled();
    virtual bool IsMouseEnabled();
    virtual void EnableMouse(bool enable);
    virtual void SetMouseClickEnabled(bool enable);
    virtual bool Unknown1E8();
    virtual bool HasMotionScript();
    virtual void QueueOnShow();
    virtual void QueueOnHide();
    virtual void OnEnter(bool motion);
    virtual void OnLeave(bool motion);
    virtual void OnMouseOverChanged(bool over);
    virtual void OnMouseFocusLost(bool clear, bool quiet);
    virtual void Unknown228();
    virtual void Unknown230(void* event);
    virtual void Unknown238();
    virtual void OnLayoutVersionChanged();
    virtual const float* GetHitRectInsets();
    virtual bool IsMouseOver(const float* point);
    virtual CRect* TransformRect(CRect* out, const CRect* rect);
    virtual bool Unknown260();
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
    void SetMouseMotionEnabled(bool enable);
    void SetInputFlag(uint8_t flag, bool set);
    bool HasInputFlags(uint8_t flags);
    void UpdateMotionRegistration();
    void UpdateClickRegistration();
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
