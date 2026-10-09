#ifndef UI_C_LAYOUT_FRAME_HPP
#define UI_C_LAYOUT_FRAME_HPP

// CLayoutFrame (Engine/Source/Frame/CLayoutFrame.cpp): the anchoring and sizing every UI
// region inherits. A frame holds up to nine anchor points, each relative to another frame's
// rect; its own rect is solved from them (or from its size where an edge is free) whenever
// it is dirty, and frames anchored to it are told to follow.

#include <cstdint>

#include "blz/Vector.hpp"

class CLayoutFrame;
class CScriptRegion;
class FrameScript_Object;

// A screen rect in UI units: bottom, left, top, right (0x10).
struct CRect {
    float minY;
    float minX;
    float maxY;
    float maxX;
};

// The "unset" coordinate (0x1426cb5e8): +infinity.
extern const float LAYOUT_UNSET;

// One anchor: an offset from a point of another frame's rect (0x18, CDataAllocator
// 0x142e99c40).
class CFramePoint {
    public:
    enum : uint8_t {
        FLAG_COMPUTING = 0x2,       // its relative rect is being read
        FLAG_LOOP = 0x4,            // a cycle was hit while computing
        FLAG_UNUSED = 0x8,          // cleared by ClearAllPoints; kept for reuse
    };

    float m_offsetX;                // 0x00
    float m_offsetY;                // 0x04
    CLayoutFrame* m_relative;       // 0x08
    uint8_t m_flags;                // 0x10
    uint8_t m_relativePoint;        // 0x14

    CFramePoint(CLayoutFrame* relative, uint8_t relativePoint, float offsetX, float offsetY);
    void OffsetBy(float x, float y);
    void ResetOffset();
    bool GetRelativeRect(CRect* rect);
    void MarkUnused();
    void SetPoint(CLayoutFrame* relative, uint8_t relativePoint, float offsetX, float offsetY);
    // The point's coordinates at a layout scale; LAYOUT_UNSET when the relative has no rect.
    float X(float scale);
    float Y(float scale);
};
static_assert(sizeof(CFramePoint) == 0x18, "CFramePoint is 0x18 bytes");

// A copy of the transform an animation applies (CAnimationData +0x00).
struct ANIMTRANSFORM {
    float m_values[12];
};

// What a running animation overrides on a frame (0x58, CDataAllocator 0x142e97a08).
struct CAnimationData {
    ANIMTRANSFORM m_transform;      // 0x00
    float m_rectOffset[8];          // 0x30
    uint8_t m_alpha;                // 0x50
    uint32_t m_flags;               // 0x54 1 transform, 2 rect offset, 4 alpha
};
static_assert(sizeof(CAnimationData) == 0x58, "CAnimationData is 0x58 bytes");

class CLayoutFrame {
    public:
    // A frame anchored to this one, and the points it anchors with (one bit per point).
    struct FRAMENODE {
        CLayoutFrame* m_frame;
        uint32_t m_points;
    };

    enum : uint16_t {
        FLAG_RECT_VALID = 0x1,
        FLAG_DIRTY = 0x2,
        FLAG_RECT_FAILED = 0x4,
        FLAG_CLAMPED = 0x8,
        FLAG_PROTECTED = 0x10,
        FLAG_PROTECT_PROPAGATED = 0x20,
        FLAG_PROTECT_SET = 0x40,
        FLAG_PROTECT_BUSY = 0x80,
        FLAG_RESIZE_QUEUED = 0x100,
        FLAG_ANCHOR_RESTRICTED = 0x200,
        FLAG_ANCHOR_ROOT = 0x400,
    };

    // m_layoutState: 1 shown (the region's own setting), 2 visible (shown with every
    // ancestor), then the recursion guards of the edge calculations.
    enum : uint32_t {
        LAYOUT_SHOWN = 0x1,
        LAYOUT_VISIBLE = 0x2,
        GUARD_LEFT = 0x4,
        GUARD_TOP = 0x8,
        GUARD_RIGHT = 0x10,
        GUARD_BOTTOM = 0x20,
        GUARD_CENTERX = 0x40,
        GUARD_CENTERY = 0x80,
    };

    static const char s_frameNodeTag[];
    static const char s_frameTag[];

    blz::embedded_vector<FRAMENODE, 16, s_frameNodeTag> m_dependents;  // 0x008
    CFramePoint* m_points[9];       // 0x120
    CRect m_rect;                   // 0x168
    CAnimationData* m_animData;     // 0x178
    float m_minResizeHeight;        // 0x180
    float m_minResizeWidth;         // 0x184
    float m_maxResizeHeight;        // 0x188
    float m_maxResizeWidth;         // 0x18c
    float m_width;                  // 0x190
    float m_height;                 // 0x194
    float m_layoutScale;            // 0x198
    float m_layoutDepth;            // 0x19c
    uint16_t m_flags;               // 0x1a0
    uint8_t m_visited;              // 0x1a2 set while GetAnchorRoot walks
    uint32_t m_layoutState;         // 0x1a4

    CLayoutFrame();
    virtual ~CLayoutFrame();
    virtual void* GetLayoutParent();
    virtual CScriptRegion* GetScriptObject();
    virtual CScriptRegion* GetScriptRegion();
    virtual void ClearAllPointsVirtual();
    virtual void OnAnchorRestrictedChanged(bool restricted);
    virtual void ScaleBy(float* scale);
    virtual void OnProtectFlagsChanged(uint16_t flag);
    virtual bool IsProtectedSelf(bool* propagated);
    virtual bool SetLayoutScale(float scale, bool force);
    virtual void SetWidth(float width);
    virtual void SetHeight(float height);
    virtual void SetSize(float width, float height);
    virtual float GetWidth();
    virtual float GetHeight();
    virtual void GetSize(float* width, float* height, bool ignoreRect);
    virtual bool CanBeQueried();
    virtual void GetClampRectInsets(float* left, float* right, float* top, float* bottom);
    virtual bool IsAttachmentOrigin();
    virtual bool CanBeAnchorFor(CLayoutFrame* frame);
    virtual CLayoutFrame* GetAnchorRoot(int32_t excludedPoint);
    // The layout frame a relativeTo name means (regions look it up as a global).
    virtual CLayoutFrame* GetRelativeByName(const char* name, CLayoutFrame* self);
    virtual bool IsLayoutReady();
    virtual void OnLayoutReady();
    // Makes the region hidden or visible as its shown setting and its parent now allow.
    virtual void ApplyHide();
    virtual void ApplyShow();
    virtual void OnLayoutDepthChanged();
    virtual void OnFrameSizeChanged(const CRect* rect, const CRect* previous);
    virtual void ClearAnimTransform();
    virtual void ClearAnimRectOffset();
    virtual void ClearAnimAlpha();

    // The frames waiting for their rect, resolved in one pass.
    static void ResizePending();
    // The pool resize-list nodes come from ("RESIZE"), made and destroyed with the UI.
    static void CreateResizeAllocator();
    static void DestroyResizeAllocator();

    // Anchors.
    int32_t SetPoint(int32_t point, CLayoutFrame* relative, int32_t relativePoint, float offsetX, float offsetY, bool now);
    int32_t SetAllPoints(CLayoutFrame* relative, bool now);
    void ClearPoint(int32_t point);
    void ClearAllPoints();
    CFramePoint* GetPoint(int32_t point);
    bool IsAnchored();
    int32_t CheckAnchorFamilyConnection(CLayoutFrame* relative, int32_t point);
    bool IsDependentOn(CLayoutFrame* frame);
    void RegisterResize(CLayoutFrame* dependent, uint32_t points);
    void UnregisterResize(CLayoutFrame* dependent, uint32_t points);
    bool OffsetPoints(float x, float y, bool now);
    bool ResetPointOffsets(bool now);
    void DestroyLayout();
    bool SnapToNearestAnchor(CLayoutFrame* target);

    // The rect.
    bool GetRect(CRect* rect);
    bool CalculateRect(CRect* rect);
    float CalculateLeft();
    float CalculateRight();
    float CalculateTop();
    float CalculateBottom();
    float CalculateCenterX();
    float CalculateCenterY();
    void Resize(bool now);
    bool UpdateRect();
    bool OnFrameResize(int32_t depth);
    void MarkDirty();
    void ClearRectFailed();
    bool CollectResizes(blz::vector<CLayoutFrame*, s_frameTag>* out);
    void AddToResizeList();
    bool PtInRect(const float* point);
    bool SizeByDrag(CLayoutFrame* relative, float dx, float dy, int32_t corner);
    bool ApplyDragSize(CLayoutFrame* relative, float width, float height, float dx, float dy);

    // Sizing, scale and clamping.
    float GetWidthActual(bool ignoreRect);
    float GetHeightActual(bool ignoreRect);
    // An optional bound: {value, set}.
    struct OPTIONALBOUND {
        float m_value;
        bool m_set;
    };
    void SetResizeBounds(float minWidth, float minHeight, const OPTIONALBOUND* maxWidth, const OPTIONALBOUND* maxHeight);
    void GetResizeBounds(float* minWidth, float* minHeight, float* maxWidth, float* maxHeight);
    bool SetLayoutDepth(float depth, bool force);
    void SetClampedToScreen(bool clamped);

    // Restriction and protection.
    bool IsAnchorRestricted();
    void SetAnchorRestricted(bool restricted);
    void SetAnchorRoot(bool root);
    bool IsProtected();
    void GetProtectedState(bool* explicitly, bool* propagated);
    void SetProtectFlag(uint16_t flag);
    void SetProtected();
    void PropagateProtectFlags(uint16_t flag);
    void PropagateProtectionTo(CLayoutFrame* target);
    void PropagateAnyProtectionTo(CLayoutFrame* target);
    void ProtectScriptRegionFrom(CLayoutFrame* relative);
    void ProtectDependents();

    // Animation overrides.
    uint8_t GetAnimAlpha();
    float* GetAnimRectOffset();
    bool HasAnimAlpha();
    bool HasAnimTransform();
    bool HasAnimRectOffset();
    void SetAnimAlpha(uint8_t alpha);
    void SetAnimTransform(const ANIMTRANSFORM* transform);
    void SetAnimRectOffset(const float* offset);

    // The Lua-facing queries (in UI units, behind the measurement restriction).
    bool GetBottom(float* bottom);
    bool GetTop(float* top);
    bool GetLeft(float* left);
    bool GetRight(float* right);
    bool GetCenter(float* x, float* y);
    bool GetRectLua(float* left, float* bottom, float* width, float* height);
    bool GetScaledRect(float* left, float* bottom, float* width, float* height);
    void GetSizeLua(bool ignoreRect, float* width, float* height);
    int32_t GetNumPoints();

    // One anchor as the API reports it (0x20).
    struct POINTINFO {
        int32_t m_point;
        int32_t m_pad04;
        CScriptRegion* m_relativeTo;
        int32_t m_relativePoint;
        float m_offsetX;
        float m_offsetY;
        int32_t m_pad1C;
    };
    bool GetPointInfo(int32_t index, POINTINFO* info);
    bool GetPointInfoByName(int32_t point, POINTINFO* info);
};
static_assert(sizeof(CLayoutFrame) == 0x1a8, "CLayoutFrame is 0x1a8 bytes");

// The restricted actions a region can be refused (FUN_14062d0c0) and why (FUN_14062d130).
enum RESTRICTEDACTION : int32_t {
    ACTION_ANCHOR_FAMILY_BRIDGING = 0,
    ACTION_FRAME_MEASUREMENT = 1,
    ACTION_CLAMPED_TO_SCREEN = 2,
    ACTION_SET_POINT = 3,
    ACTION_SET_MOVABLE = 4,
    ACTION_SET_RESIZABLE = 5,
};
const char* RestrictedActionToString(int32_t action);
const char* RestrictedReasonToString(int32_t reason);
// Why `region` may not do `action` (0 or 1 allowed, otherwise a reason).
int32_t ScriptRegion_CheckRestriction(int32_t action, CScriptRegion* region);
// Whether the action is allowed; a refusal is left as the pending script error.
bool ScriptRegion_CheckAction(int32_t action, CScriptRegion* region, const char* method);
// Raises the refusal as a Lua error.
void ScriptRegion_RaiseRestriction(int32_t action, int32_t reason, CScriptRegion* region, const char* method, struct lua_State* L);
// Whether the action is restricted at all for code running now.
bool ScriptRegion_IsActionRestricted(int32_t action);

#endif
