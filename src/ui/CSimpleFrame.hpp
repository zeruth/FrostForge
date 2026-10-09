#ifndef UI_C_SIMPLE_FRAME_HPP
#define UI_C_SIMPLE_FRAME_HPP

// CSimpleFrame (Engine/Source/Frame/CSimpleFrame.cpp): the frame every UI widget is built on
// ("Frame", 0x300 bytes; vtable 0x1426ca410, CLayoutFrame vtable 0x1426ca728 at +0x18,
// CAnimatableObject vtable 0x1426ca820 at +0x1d8). A frame owns regions (textures and font
// strings, drawn by layer) and child frames, sits in a strata and level, and carries the
// attributes, the drag and the input handling of the widgets built on it.

#include <cstdint>

#include "storm/hash/TSHashTable.hpp"
#include "storm/list/TSLink.hpp"
#include "blz/Vector.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CScriptRegion.hpp"

class CRenderBatch;
class CSimpleFrame;
class CSimpleRegion;
class CScriptStatus;

// The script-visible children and regions of a frame, as GetChildren and GetRegions list them.
extern const char s_simpleFrameListTag[];
extern const char s_simpleRegionListTag[];
typedef blz::embedded_vector<CSimpleFrame*, 32, s_simpleFrameListTag> SIMPLEFRAMELIST;
typedef blz::embedded_vector<CSimpleRegion*, 32, s_simpleRegionListTag> SIMPLEREGIONLIST;

// A child in a frame's child list (SIMPLEFRAMENODE, 0x18).
struct SIMPLEFRAMENODE {
    TSLink<SIMPLEFRAMENODE> m_link;     // 0x00
    CSimpleFrame* m_frame;              // 0x10
};
static_assert(sizeof(SIMPLEFRAMENODE) == 0x18, "a child node is 0x18 bytes");

// One attribute (FRAMEATTR, HASHKEY_STRI): the value is a registry reference.
struct FRAMEATTR : TSHashObject<FRAMEATTR> {
    char* m_str;                        // 0x30
    int32_t m_ref;                      // 0x38 -1 for nil
    const char* m_taint;                // 0x40 the taint it was set under
};
static_assert(sizeof(FRAMEATTR) == 0x48, "FRAMEATTR is 0x48 bytes");

// TSHashTable<FRAMEATTR, HASHKEY_STRI, 8> (vtable 0x1426ca3f0).
class FRAMEATTRTABLE : public TSHashTable<FRAMEATTR> {
    public:
    // ref: FUN_140616d60
    void Delete(FRAMEATTR* node) override;
    // ref: FUN_140616d90
    FRAMEATTR* NewNode(TSExplicitList<FRAMEATTR>* slot, uint32_t extra, uint32_t flags) override;
    // ref: FUN_140619e80
    FRAMEATTR* Find(const char* name);
    // ref: FUN_140618820
    FRAMEATTR* Add(const char* name, uint32_t extra, uint32_t flags);
};

class CSimpleFrame : public CScriptRegion, public CAnimatableObject {
    public:
    // m_frameFlags.
    enum : uint32_t {
        FRAME_TOPLEVEL = 0x1,
        FRAME_MOVABLE = 0x2,
        FRAME_RESIZABLE = 0x4,
        FRAME_UNKNOWN_8 = 0x8,
        FRAME_USER_PLACED = 0x10,
        FRAME_IN_SCROLL_FRAME = 0x20,       // a scroll frame's descendant (inherited)
        FRAME_SCROLL_CHILD = 0x40,          // set by the scroll frame itself, not inherited
        FRAME_INHERITED_200 = 0x200,
        FRAME_DONT_SAVE_POSITION = 0x400,
        FRAME_USE_PARENT_LEVEL = 0x800,
        FRAME_PROPAGATE_KEYBOARD = 0x1000,
        FRAME_BATCH_DIRTY = 0x2000,
        FRAME_OWN_BATCH = 0x4000,
        FRAME_FLATTEN_RENDER_LAYERS = 0x8000,
        FRAME_BATCHED = 0x10000,            // drawn inside an ancestor's own batch
        FRAME_HYPERLINKS_ENABLED = 0x20000,
        FRAME_PROPAGATE_HYPERLINKS = 0x40000,
        FRAME_CLIP_CHILDREN = 0x80000,
        FRAME_IGNORE_PARENT_ALPHA = 0x100000,
        FRAME_IGNORE_PARENT_SCALE = 0x200000,
        FRAME_FIXED_STRATA = 0x400000,
        FRAME_FIXED_LEVEL = 0x800000,
        FRAME_HIGHLIGHT_LOCKED = 0x1000000,
        FRAME_DESTROYED = 0x2000000,
        FRAME_HAS_ONUPDATE = 0x4000000,
    };

    // m_flags2: bits 0-4 the draw layers drawn, the strata in bits 7-10.
    enum : uint32_t {
        FRAME2_LAYERS = 0x1f,
        FRAME2_DRAGGING = 0x20,
        FRAME2_NO_SIZE_SCRIPT = 0x40,
        FRAME2_STRATA = 0x780,
        FRAME2_DRAG_PENDING = 0x1000,
        FRAME2_FRAME_BUFFER = 0x2000,
    };

    // The frame whose XML is being loaded (0x142e9a070, set by FUN_140614c20).
    static CSimpleFrame* s_loading;
    // The number of frames alive (0x142e97b28).
    static int64_t s_count;
    // The registry references of the frames' script metatables, open and restricted
    // (0x142cce840, 0x142cce844).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t m_id;                           // 0x1e8
    uint32_t m_frameFlags;                  // 0x1ec
    TSLink<CSimpleFrame> m_updateLink;      // 0x1f0 CSimpleTop's update list
    TSLink<CSimpleFrame> m_rootLink;        // 0x200 CSimpleTop's root list
    TSExplicitList<CSimpleRegion> m_drawRegions;    // 0x210 the visible regions (link +0x200)
    TSExplicitList<SIMPLEFRAMENODE> m_children;     // 0x228
    TSExplicitList<CSimpleRegion> m_regions;        // 0x240 every region (link +0x1f0)
    float m_scale;                          // 0x258
    float m_depth;                          // 0x25c
    float m_effectiveDepth;                 // 0x260
    uint32_t m_pad264;
    CSimpleFrame* m_frameBuffer;            // 0x268 the frame-buffer frame it renders into
    uint32_t m_dragButtons;                 // 0x270
    float m_dragStartX;                     // 0x274
    float m_dragStartY;                     // 0x278
    uint32_t m_buttonsDown;                 // 0x27c
    CRect m_hitRectInsets;                  // 0x280 bottom, left, top, right
    CRect m_clampRectInsets;                // 0x290
    FRAMEATTRTABLE m_attributes;            // 0x2a0
    uint8_t m_alpha;                        // 0x2f0
    uint8_t m_parentAlpha;                  // 0x2f1
    uint16_t m_level;                       // 0x2f2
    int32_t m_raiseOrder;                   // 0x2f4 among toplevel frames
    uint32_t m_flags2;                      // 0x2f8
    uint32_t m_pad2FC;

    // ref: FUN_140613290
    explicit CSimpleFrame(CSimpleFrame* parent);
    // ref: FUN_140613850
    ~CSimpleFrame() override;
    // Frames come from their own pool (0x142e97b38, 100 per block).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id (0x142e979f4).
    static int32_t GetObjectType();
    // The metatables every frame's table gets.
    static void CreateScriptMetaTables();
    static void DestroyScriptMetaTables();

    // ---- FrameScript_Object / CScriptObject / CScriptRegion ----------------------------
    int32_t GetScriptMetaTable() override;
    void OnScriptsChanged(uint8_t index, bool hasScript) override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    void GetScriptCPUUsage(lua_State* L, double* time, int32_t* calls, bool includeChildren) override;
    void OnProtected() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    CAnimatableObject* GetAnimatableObject() override;
    void DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) override;
    void Unknown98(void* argument) override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void PostLoadXML(XMLOBJECT* node, CStatus* status) override;
    void SetParentArg(PARENTARG* parent) override;
    void Hide() override;
    void Show() override;
    void SetShown(bool shown) override;
    void SetAlpha(uint8_t alpha) override;
    uint8_t GetAlpha() override;
    uint8_t GetEffectiveAlpha() override;
    bool IsDragging() override;
    bool OnMouseMove(MOUSEEVENT* event) override;
    uint64_t GetDrawOrderKey(int32_t layer, int32_t subLevel) override;
    uint64_t GetDrawOrder() override;
    bool IsBatched() override;
    bool HasCharHandler() override;
    bool HasKeyHandler() override;
    bool HasPadButtonHandler(void* event) override;
    bool HasPadStickHandler(void* event) override;
    bool HasWheelHandler() override;
    bool OnChar(int32_t key) override;
    bool OnKeyDown(void* event) override;
    bool OnKeyUp(void* event) override;
    void OnMouseDown(MOUSEEVENT* event, const char* button) override;
    void OnMouseUp(MOUSEEVENT* event, const char* button) override;
    bool OnMouseWheel(MOUSEEVENT* event) override;
    bool OnPadButtonDown(void* event) override;
    bool OnPadButtonUp(void* event) override;
    bool OnPadStick(void* event) override;
    CScriptRegion* GetMouseTarget(int32_t type, MOUSEEVENT* event, bool* handled, bool force) override;
    bool GetHyperlinksPropagated() override;
    bool GetHyperlinksEnabled() override;
    void QueueOnShow() override;
    void QueueOnHide() override;
    void OnEnter(bool motion) override;
    void OnLeave(bool motion) override;
    void OnMouseFocusLost(bool clear, bool quiet) override;
    void OnDragStart(MOUSEEVENT* event) override;
    void OnDragStop(MOUSEEVENT* event) override;
    void OnReceiveDrag(MOUSEEVENT* event) override;
    void OnLayoutVersionChanged() override;
    const float* GetHitRectInsets() override;
    CRect* TransformRect(CRect* out, const CRect* rect) override;
    bool IsInScrollFrame() override;
    void OnScriptHandlerLoaded(XMLOBJECT* handler) override;
    void RunOnLoad() override;

    // ---- the frame's own slots (0x280..0x310) ------------------------------------------
    virtual void Unknown280();
    // Names the frame and takes its id, before the rest of its XML loads.
    virtual void LoadXMLName(XMLOBJECT* node, CStatus* status);
    virtual void RemoveRegion(CSimpleRegion* region);
    virtual bool IsChildShown(CScriptRegion* child);
    virtual void RunOnUpdate(float elapsed);
    // Whether the frame needs updating every frame without an OnUpdate handler.
    virtual bool WantsUpdate();
    // Adds the visible regions of the drawn layers to a batch.
    virtual void AddRegionsToBatch(CRenderBatch* batch);
    virtual void Unknown2B8();
    virtual void Unknown2C0();
    virtual void Unknown2C8();
    virtual void OnSizeChanged(float width, float height);
    virtual void LockHighlight(bool lock);
    virtual void Unknown2E0();
    // Recomputes the layout scale from the frame's scale and its parent's.
    virtual bool UpdateScale(bool force);
    virtual void UpdateEffectiveAlpha();
    virtual void AddChild(CSimpleFrame* child);
    virtual void RemoveChild(CSimpleFrame* child);
    // Whether the frame draws into a batch of its own.
    virtual bool NeedsOwnBatch();
    // Rebuilds the frame's own batch.
    virtual void RenderToBatch(CRenderBatch* batch);

    // ---- CLayoutFrame ------------------------------------------------------------------
    void OnAnchorRestrictedChanged(bool restricted) override;
    void ScaleBy(float* scale) override;
    void OnProtectFlagsChanged(uint16_t flag) override;
    bool IsProtectedSelf(bool* propagated) override;
    void GetClampRectInsets(float* left, float* right, float* top, float* bottom) override;
    bool IsLayoutReady() override;
    void OnLayoutReady() override;
    void ApplyHide() override;
    void ApplyShow() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;

    // ---- CAnimatableObject --------------------------------------------------------------
    void Unknown08() override;
    void Unknown10() override;
    void StopAnimating() override;
    void OnAnimStarted(bool alpha, bool transform, bool rectOffset) override;
    void OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset) override;
    void Unknown40(void* value) override;
    void Unknown48(void* value) override;
    void Unknown50(void* value) override;
    void Unknown58(int32_t index, void* value, float extra) override;
    void Unknown60(int32_t index, void* first, void* second) override;
    void Unknown68(int32_t index, void* first, void* second) override;
    void SetAnimAlpha(uint8_t alpha) override;
    CScriptRegion* GetScriptRegion() override;

    // ---- lifetime ------------------------------------------------------------------------
    // Takes the frame and everything under it down (regions, children, attributes).
    void Destroy();

    // ---- hierarchy and regions -----------------------------------------------------------
    void SetParent(CSimpleFrame* parent);
    void AddRegion(CSimpleRegion* region);
    void AddDrawRegion(CSimpleRegion* region);
    void RemoveDrawRegion(CSimpleRegion* region);
    // The region or child its parentKey names.
    CScriptObject* FindChildByParentKey(const char* key);
    int32_t GetNumChildren();
    int32_t GetNumRegions();
    SIMPLEFRAMELIST* GetScriptChildren(SIMPLEFRAMELIST* children);
    SIMPLEREGIONLIST* GetScriptRegions(SIMPLEREGIONLIST* regions);
    void DesaturateHierarchy(float desaturation, bool excludeRoot);
    void DesaturateRegions(float desaturation, uint32_t depth, uint32_t firstDepth);
    void RotateTextures(float radians, float x, float y);

    // ---- strata, level, raising ----------------------------------------------------------
    void SetFrameStrata(uint32_t strata);
    void SetFrameStrataChecked(uint32_t strata);
    void ClearFrameStrata();
    void SetFrameLevel(uint16_t level);
    void SetFrameLevelChecked(uint16_t level);
    void SetRaiseOrder(int32_t order);
    void Raise();
    void Lower();
    void SetToplevel(bool toplevel);

    // ---- batching ------------------------------------------------------------------------
    void MarkBatchDirty();
    void MarkParentBatchDirty();
    void MarkScrollRangeDirty();
    void UpdateOwnBatch();
    void UpdateBatched();
    void SetBatched(bool batched);
    void SetFrameBufferOwner(CSimpleFrame* owner);
    void SetIsFrameBuffer(bool frameBuffer);
    void SetFlattenRenderLayers(bool flatten);
    void UpdateClipRect();
    void RefreshBatch();
    void RefreshRegions();
    void RenderChildren(CRenderBatch* batch);
    void AddToParentBatch(CRenderBatch* batch);
    void RebuildBatchIfDirty();

    // ---- alpha, scale, depth -------------------------------------------------------------
    bool SetScale(float scale, bool force);
    bool SetDepth(float depth, bool force);
    bool UpdateDepth(bool force);
    void SetIgnoreParentAlpha(bool ignore);
    void SetIgnoreParentScale(bool ignore);

    // ---- draw layers, input and flags ------------------------------------------------------
    void EnableDrawLayer(uint8_t layer);
    void DisableDrawLayer(uint8_t layer);
    void SetDrawLayerEnabled(uint8_t layer, bool enabled);
    void EnableKeyboard(bool enable);
    void EnableGamePadButton(bool enable);
    void EnableGamePadStick(bool enable);
    bool IsKeyboardEnabled();
    bool IsGamePadButtonEnabled();
    bool IsGamePadStickEnabled();
    void SetFrameFlag(uint32_t flag, bool set);
    void SetMovable(bool movable);
    void SetResizable(bool resizable);
    void SetUserPlaced(bool placed);
    void SetHyperlinksEnabled(bool enabled);
    void UpdateHyperlinkRegistration();
    void SetInScrollFrame(bool inScrollFrame, int32_t mode);
    void SetInherited200(bool set);
    void UpdateUpdateRegistration();
    bool WantsUpdateCheck();
    void SetHitRectInsets(float left, float right, float top, float bottom);
    void SetClampRectInsets(float left, float right, float top, float bottom);
    void GetHitRectInsetsLua(float* left, float* right, float* top, float* bottom);

    // ---- dragging ------------------------------------------------------------------------
    void RegisterForDrag(const uint32_t* buttons, uint64_t count);
    void StartMoving(bool alwaysStartFromMouse);
    void StartSizing(int32_t corner, bool alwaysStartFromMouse);
    void StartSizingChecked(const int32_t* corner, bool alwaysStartFromMouse);
    void StopMovingOrSizing();
    void CancelDrag();
    void InterceptStartDrag(CSimpleFrame* other);

    // ---- attributes ------------------------------------------------------------------------
    bool GetAttribute(const char* name, int32_t* ref, const char** taint);
    void SetAttribute(const char* name, int32_t ref, const char* taint, bool quiet);
    void RunOnAttributeChanged(const char* name, int32_t ref);

    // ---- bounds ----------------------------------------------------------------------------
    int32_t CalculateBounds(CRect* rect);
    CRect* GetBoundsRect(CRect* rect);
    void GetBoundsRectLua(float* left, float* bottom, float* width, float* height);

    // ---- hyperlinks ------------------------------------------------------------------------
    void RunOnHyperlinkClick(const FRAMESCRIPT_LSTRING* link, const FRAMESCRIPT_LSTRING* text, const char* button,
        CSimpleRegion* region, const CRect* rect);
    void RunOnHyperlinkEnter(const FRAMESCRIPT_LSTRING* link, const FRAMESCRIPT_LSTRING* text, CSimpleRegion* region,
        const CRect* rect);
    void RunOnHyperlinkLeave();

    // ---- XML -------------------------------------------------------------------------------
    void LoadXMLAttributes(XMLOBJECT* node, CStatus* status);
    void LoadXMLLayers(XMLOBJECT* node, CStatus* status);
};
static_assert(sizeof(CSimpleFrame) == 0x300, "CSimpleFrame is 0x300 bytes");

// Creates the animation groups of an element's <Animations> on an object.
// ref: FUN_14062fc00
void CSimpleFrame_LoadAnimations(CScriptRegion* object, XMLOBJECT* node, CStatus* status);

// The names of keys ("A", "ENTER", "NUMPAD1", ...) and of gamepad buttons ("PADDUP", ...).
const char* KeyToString(int32_t key);
const char* GamePadButtonToString(uint8_t button);

#endif
