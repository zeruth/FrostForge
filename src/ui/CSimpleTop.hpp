#ifndef UI_C_SIMPLE_TOP_HPP
#define UI_C_SIMPLE_TOP_HPP

// CSimpleTop (Engine/Source/Frame/CSimpleTop.cpp): the root of the UI. It is the layout frame
// the screen is (0.8 x 0.6), keeps the frames without a parent, dispatches mouse, keyboard and
// gamepad input to the regions registered for it, queues scripts, and renders the frame
// batches (0x1070 bytes, vtable 0x1426c7718).

#include <cstdint>
#include <memory>

#include "blz/ChainedHash.hpp"
#include "blz/RbTree.hpp"
#include "blz/Vector.hpp"
#include "gxu/TextBlock.hpp"
#include "storm/list/TSList.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CRenderBatch.hpp"

class CAnimatableObject;
class CRenderBatch;
class CScriptRegion;
class CSimpleFrame;
class FrameScript_Object;

// What a mouse event carries (0x24).
struct MOUSEEVENT {
    int32_t m_type;                 // 0x00
    int32_t m_button;               // 0x04 MOUSEBUTTON bit
    int32_t m_unknown08;            // 0x08
    int32_t m_unknown0C;            // 0x0C
    float m_x;                      // 0x10 UI units once received
    float m_y;                      // 0x14
    int32_t m_unknown18;            // 0x18
    int32_t m_time;                 // 0x1C
    int32_t m_flags;                // 0x20
};
static_assert(sizeof(MOUSEEVENT) == 0x24, "MOUSEEVENT is 0x24 bytes");

// A registration change made while input was being dispatched (0x10).
struct DEFERREDINPUTEVENTCHANGE {
    CScriptRegion* m_region;
    int32_t m_type;
    bool m_register;
};
static_assert(sizeof(DEFERREDINPUTEVENTCHANGE) == 0x10, "a deferred input change is 0x10 bytes");

// blz's chained hash table header (count of buckets, buckets, size, load limit).
typedef blz::chained_hash_header CHAINEDHASH;

// blz::map's header (0x28): a node whose parent is the root, left the leftmost node and right
// the rightmost, then the size.
struct RBTREEHEADER : blz::rb_node_base {
    uint64_t m_size;
};
static_assert(sizeof(RBTREEHEADER) == 0x28, "a blz::map header is 0x28 bytes");

extern const char s_regionVectorTag[];
extern const char s_deferredScriptTag[];
extern const char s_frameVectorTag[];
extern const char s_deferredInputTag[];

// A script queued to run when the current batch of shows and hides is over (0x10).
struct DEFERREDSCRIPT {
    FrameScript_Object* m_object;
    uint8_t m_index;
};

// DeferredScriptBuffer (0x38): while a scope is open, OnShow and OnHide wait in the queue; when
// the last scope closes they run, round after round, until no more are queued (twelve
// rounds at most).
class DeferredScriptBuffer {
    public:
    int32_t m_depth = 0;                                                // 0x00
    blz::vector<DEFERREDSCRIPT, s_deferredScriptTag> m_running;         // 0x08
    blz::vector<DEFERREDSCRIPT, s_deferredScriptTag> m_queued;          // 0x20

    // ref: FUN_140317cb0
    void Begin() {
        this->m_depth++;
    }
    void End();
    // Runs the script now when no scope is open, otherwise queues it.
    void Add(FrameScript_Object* object, uint8_t index);
    void Flush();
};
static_assert(sizeof(DeferredScriptBuffer) == 0x38, "DeferredScriptBuffer is 0x38 bytes");

// A key event as the client hands it to the UI (0x14).
struct KEYEVENT {
    int32_t m_key;                  // 0x00 the engine's key code, -1 for none
    uint32_t m_time;                // 0x04
    uint8_t m_repeat;               // 0x08 the OS's previous-state bit
    uint8_t m_system;               // 0x09 a system key (alt held)
    uint16_t m_pad0a;
    uint32_t m_virtualKey;          // 0x0C
    float m_scan;                   // 0x10
};
static_assert(sizeof(KEYEVENT) == 0x14, "KEYEVENT is 0x14 bytes");

class CSimpleTop : public CLayoutFrame {
    public:
    // The one root (0x142e979d8).
    static CSimpleTop* s_instance;
    // Only what is drawn on top shows (DAT_142e979d0).
    static uint8_t s_drawOnTopOnly;

    CSimpleFrame* m_dragFrame;                                          // 0x1a8
    int32_t m_dragType;                                                 // 0x1b0
    int32_t m_dragCorner;                                               // 0x1b4
    float m_dragX;                                                      // 0x1b8
    float m_dragY;                                                      // 0x1bc
    int32_t m_raiseCounter;                                             // 0x1c0 the last raise order handed out
    uint8_t m_strataDirty;                                              // 0x1c4
    CRenderBatch m_renderBatch;                                         // 0x1c8 the root frames' batch
    CHAINEDHASH m_frameBatches;                                         // 0x230 CSimpleFrame* -> CRenderBatch*
    blz::vector<CSimpleFrame*, s_frameVectorTag> m_batchUpdates;        // 0x250
    CHAINEDHASH m_layoutWaiting;                                        // 0x268 CLayoutFrame*
    uint8_t m_imeEnabled;                                               // 0x288 an edit box has the focus
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> m_mouseOver;    // 0x290
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> m_mouseMotion;  // 0x2b8
    RBTREEHEADER m_buttonFocus;                                         // 0x2e0 MOUSEBUTTON -> regions
    CScriptRegion* m_mouseFocus;                                        // 0x308
    uint8_t m_motionTracking;                                           // 0x310
    CScriptRegion* m_unknown318;                                        // 0x318
    uint32_t m_unknown320;                                              // 0x320
    uint8_t m_unknown324;                                               // 0x324
    CScriptRegion* m_keyFocus[310];                                     // 0x328 per key
    CScriptRegion* m_padFocus[32];                                      // 0xcd8 per gamepad button
    blz::vector<CScriptRegion*, s_regionVectorTag> m_inputRegions[7];   // 0xdd8 per input event type
    int32_t m_dispatchDepth;                                            // 0xe80
    blz::vector<DEFERREDINPUTEVENTCHANGE, s_deferredInputTag> m_deferredInput;  // 0xe88
    blz::vector<CSimpleFrame*, s_frameVectorTag> m_updateChecks;        // 0xea0
    blz::vector<CSimpleFrame*, s_frameVectorTag> m_shownChanges;        // 0xeb8
    int32_t m_shownRootCount;                                           // 0xed0
    uint8_t m_mouseFocusDirty;                                          // 0xed4
    MOUSEEVENT m_mouseEvent;                                            // 0xed8
    MOUSEEVENT m_lastMouseEvent;                                        // 0xefc
    const char* m_currentButton;                                        // 0xf20
    void* m_mouseDownHook;                                              // 0xf28
    void* m_mouseMoveHook;                                              // 0xf30
    void* m_unknownF38;                                                 // 0xf38
    void* m_keyFilterHook;                                              // 0xf40
    void* m_keyCaptureHook;                                             // 0xf48
    void* m_keyDownHook;                                                // 0xf50
    void* m_padHook;                                                    // 0xf58
    uint8_t m_secureLoad;                                               // 0xf60
    uint8_t m_unknownF61;                                               // 0xf61
    uint8_t m_inMouseScript;                                            // 0xf62
    uint8_t m_inputSuspended;                                           // 0xf63
    uint8_t m_resizing;                                                 // 0xf64
    // Called with a protected frame that tainted code tried to change.
    void (*m_actionBlockedHook)(CSimpleFrame* frame);                   // 0xf68
    void (*m_preUpdateHook)();                                          // 0xf70
    DeferredScriptBuffer m_deferredScripts;                             // 0xf78
    TSList<CSimpleFrame, TSGetLink<CSimpleFrame>, 8> m_rootFrames;      // 0xfb0 linked through +0x200
    TSList<CSimpleFrame, TSGetLink<CSimpleFrame>, 8> m_updateFrames;    // 0xfc8 linked through +0x1f0
    CHAINEDHASH m_animating;                                            // 0xfe0 CAnimatableObject*
    CHAINEDHASH m_regionLayoutVersions;                                 // 0x1000 CScriptRegion* -> int
    int32_t m_layoutVersion;                                            // 0x1020
    uint8_t m_layoutDirty;                                              // 0x1024
    float m_projection[16];                                             // 0x1028
    uint8_t m_inputPhase;                                               // 0x1068 1 key, 2 mouse, 3 gamepad

    // The UI root (FUN_1405ebe10); `preUpdate` runs before each update.
    explicit CSimpleTop(void (*preUpdate)());
    ~CSimpleTop() override;

    // Whether templates load under their own taint even without a frame being loaded.
    bool IsSecureLoad();

    // The IME on or off, as the edit focus comes and goes (FUN_1405ee420).
    void EnableIme(bool enable);

    // Input registration (types: 0..6; 2 motion, 3 click, 4 wheel).
    void RegisterForInput(CScriptRegion* region, int32_t type);
    void UnregisterForInput(CScriptRegion* region, int32_t type, bool quiet);
    void DeferInputChange(CScriptRegion* region, int32_t type, bool add);
    void ApplyDeferredInputChanges();
    // A region is going away: it leaves the mouse lists and the layout-version map.
    void OnRegionDestroyed(CScriptRegion* region);
    // A region stops receiving input events.
    void RemoveRegionInput(CScriptRegion* region, bool quiet);
    // Queues a script to run after the current update (the DeferredScriptBuffer).
    void QueueScript(FrameScript_Object* object, int32_t index);
    int32_t GetRegionLayoutVersion(CScriptRegion* region);
    // A frame waits for its layout until its objects are ready (FUN_1405edde0, FUN_1405f3390).
    void AddLayoutWaiting(CLayoutFrame* frame);
    void RemoveLayoutWaiting(CLayoutFrame* frame);

    // Root frames.
    void AddRootFrame(CSimpleFrame* frame);
    void MarkStrataDirty();
    void InvalidateLayout();
    void AddUpdateCheck(CSimpleFrame* frame);
    void AddShownChange(CSimpleFrame* frame);
    void AddPendingBatchUpdate(CSimpleFrame* frame);
    void AddHiddenChange(CSimpleFrame* frame);
    // A frame is being destroyed: it leaves the root lists and the drag.
    void RemoveFrame(CSimpleFrame* frame);
    void OnFrameSizeChanged(CSimpleFrame* frame);

    // The batches of frames that draw into their own.
    CRenderBatch* GetFrameBatch(CSimpleFrame* frame);
    CRenderBatch* FindFrameBatch(CSimpleFrame* frame);
    void RemoveFrameBatch(CSimpleFrame* frame);

    // The objects with animations playing.
    void AddAnimating(CAnimatableObject* object);
    void RemoveAnimating(CAnimatableObject* object);

    // Dragging a frame to move or size it.
    bool StartDrag(CSimpleFrame* frame, int32_t type, float x, float y, int32_t corner);
    void StopDrag();

    // The regions the mouse buttons went down on.
    bool IsInButtonCapture(CScriptRegion* region);
    bool IsAnyButtonCaptured();
    bool AddButtonCapture(CScriptRegion* region);
    bool ReleaseButtonCapture(CScriptRegion* region);
    // Whether `b` draws above `a`.
    bool IsRegionAbove(CScriptRegion* a, CScriptRegion* b);
    // The dragged frame follows the mouse.
    void UpdateMouseFocus(MOUSEEVENT* event);
    // The regions registered for `type` under the mouse, topmost first.
    void FindBestMouseFoci(blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>* out, int32_t type,
        MOUSEEVENT* event, bool force, blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>* entered);
    // The mouse events from the client (positions 0..1 of the window, y up).
    bool OnMouseMoveEvent(MOUSEEVENT* event, bool real);
    void OnMouseDownEvent(MOUSEEVENT* event);
    void OnMouseUpEvent(MOUSEEVENT* event);
    bool OnMouseWheelEvent(MOUSEEVENT* event);
    bool OnMouseDeltaEvent(MOUSEEVENT* event);
    // The keyboard events from the client: a character to the frames that take characters, a
    // key to those that take keys (the one that takes it keeps the key until it is let go).
    bool OnCharEvent(uint32_t character);
    bool OnKeyDownEvent(KEYEVENT* event);
    bool OnKeyDownRepeatEvent(KEYEVENT* event);
    bool OnKeyUpEvent(KEYEVENT* event);

    // The UI's frame: animations, layout, OnUpdate scripts and the mouse focus.
    void OnUpdate(float elapsed);
    // Draws the UI: the dirty batches rebuilt, the root batch rebuilt when the strata changed,
    // then drawn into the window's frame buffer (FUN_1405f09a0, vtable slot 0xf8).
    void Render();
    // Frames whose layout was waiting on their objects get it once they are ready.
    void ProcessLayoutWaiting();
    // Roots that were shown move to the front of the root list, hidden ones to the back.
    void ProcessShownChanges();
    // Frames join or leave the update list as they come to want OnUpdate or not.
    void ProcessUpdateChecks();
};

class CTexture;
// Lets a UI texture go (FUN_1405f3c30); the reference keeps recently released ones a while.
void UiReleaseTexture(CTexture** texture);
// The unneeded-texture cache: its limits and heap, the per-frame trim, and the teardown.
void UiUnneededTexturesInitialize();
void UiTrimUnneededTextures(bool all);
void UiUnneededTexturesShutdown();

// The cursor in UI units (FUN_140236a40).
void UiGetCursorPosition(float* x, float* y);

// A font handle whose last user went; it is dropped at the next UI update.
void UiAddUnneededFontHandle(const HTEXTFONT* handle);
void UiReleaseUnneededFontHandles();

// GLOBAL_MOUSE_DOWN / GLOBAL_MOUSE_UP to the scripts (FUN_141174d40, FUN_141174d80).
void UiSignalGlobalMouseDown(const char* button);
void UiSignalGlobalMouseUp(const char* button);

#endif
