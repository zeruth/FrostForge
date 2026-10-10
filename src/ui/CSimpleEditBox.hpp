#ifndef UI_C_SIMPLE_EDIT_BOX_HPP
#define UI_C_SIMPLE_EDIT_BOX_HPP

// Engine/Source/Frame/CSimpleEditBox ("EditBox", 0x488 bytes; vtable 0x14297d138, CLayoutFrame
// vtable 0x14297d450 at +0x18, CAnimatableObject vtable 0x14297d548 at +0x1d8, CSimpleFontedFrame
// vtable 0x14297d5e0 at +0x300, CSimpleIMEHost vtable 0x14297d5e8 at +0x308).
//
// The text lives in m_text with one m_textInfo entry per byte. The first byte of each token
// (a character, or a control code GxuDetermineControlCode found) holds its byte length in the
// low 16 bits, its kind in bits 16..23, and 0x80000000 while inside a hyperlink (kind 5 opens,
// 6 closes) or 0x40000000 while inside a texture (9 opens, 10 closes); the token's other bytes
// hold 0. Kinds 1, 5, 7, 9 and 13 open a code and travel with the character after them, kinds
// 2, 6, 8, 10, 12 and 14 close one and travel with the character before; 0, 3 and 4 are the
// letters counted against the letter limits.

#include <cstdint>

#include "ui/CSimpleFontedFrame.hpp"
#include "ui/CSimpleFrame.hpp"

class CSimpleFontString;
class CSimpleTexture;
class CTexture;
struct MOUSEEVENT;

// A native listener an edit box tells about a keypress or a text change (the six at +0x400;
// the event it gets carries vtable 0x14297d648, the listener's id and the edit box).
class CSimpleEditBoxListener {
    public:
    struct EVENT {
        const void* m_vtable;
        uint32_t m_unknown08;
        uint32_t m_id;
        class CSimpleEditBox* m_editBox;
    };
    virtual void Unknown00();
    virtual void Unknown08();
    virtual void Unknown10();
    virtual void Unknown18();
    virtual void OnEditBoxEvent(EVENT* event);
};

class CSimpleEditBox : public CSimpleFrame, public CSimpleFontedFrame, public CSimpleIMEHost {
    public:
    // m_editFlags.
    enum : uint32_t {
        EDIT_AUTO_FOCUS = 0x1,
        EDIT_MULTI_LINE = 0x2,
        EDIT_NUMERIC = 0x4,
        EDIT_PASSWORD = 0x8,
        EDIT_ALT_ARROW_KEY_MODE = 0x10,
        EDIT_COUNT_INVISIBLE_LETTERS = 0x20,
        EDIT_SECURITY_DISABLE_SET_TEXT = 0x40,
        EDIT_SECURITY_DISABLE_PASTE = 0x80,
        EDIT_SECURE_TEXT = 0x100,
        EDIT_ALPHABETIC_ONLY = 0x200,
        EDIT_NUMERIC_FULL_RANGE = 0x400,
        // What the input modes clear when one of them is set.
        EDIT_INPUT_MODES = 0x60c,
    };

    // m_dirtyFlags.
    enum : uint32_t {
        DIRTY_TEXT = 0x2,
        DIRTY_HIGHLIGHT = 0x4,
        DIRTY_CURSOR = 0x8,
        DIRTY_USER_INPUT = 0x10,
        DIRTY_DRAG = 0x20,
        DIRTY_LINES = 0x40,
    };

    // A highlight quad: four {x, y, z} corners (0x30).
    struct HighlightTextureInfo {
        float m_vertices[4][3];
    };

    // A history line: the text and the taint it came with (0x10).
    struct EditHistory {
        char* m_text;
        const char* m_taint;
    };

    uint32_t m_editFlags;                       // 0x318
    uint32_t m_dirtyFlags;                      // 0x31c
    uint8_t m_cursorContext;                    // 0x320 the input phase the cursor last moved in
    uint8_t m_pad321[7];
    const char* m_textTaint;                    // 0x328
    CSimpleFontedFrameFont* m_font;             // 0x330
    CSimpleFontString* m_string;                // 0x338
    char* m_text;                               // 0x340
    uint32_t* m_textInfo;                       // 0x348
    char* m_passwordText;                       // 0x350 the stars a password shows
    int32_t m_textLength;                       // 0x358
    int32_t m_maxBytes;                         // 0x35c the last byte index allowed, -1 for any
    int32_t m_maxLetters;                       // 0x360
    int32_t m_maxMarkupBytes;                   // 0x364
    int32_t m_visibleTextByteLimit;             // 0x368
    int32_t m_textCapacity;                     // 0x36c
    int32_t m_visibleStart;                     // 0x370
    int32_t m_visibleLength;                    // 0x374
    HighlightTextureInfo* m_highlights;         // 0x378
    uint64_t m_highlightCount;                  // 0x380
    uint64_t m_highlightCapacity;               // 0x388 the high bit marks borrowed storage
    CTexture* m_highlightTexture;               // 0x390 a white texture the quads use
    uint32_t m_highlightColor;                  // 0x398
    int32_t m_highlightStart;                   // 0x39c
    int32_t m_highlightEnd;                     // 0x3a0
    uint32_t m_dragTime;                        // 0x3a4 when a drag-select next scrolls
    CSimpleTexture* m_cursor;                   // 0x3a8
    int32_t m_cursorPos;                        // 0x3b0
    float m_blinkSpeed;                         // 0x3b4
    float m_blinkElapsed;                       // 0x3b8
    uint32_t m_pad3bc;
    // TSGrowableArray<unsigned int, 31>: the byte each wrapped line starts at, and the end.
    uint32_t m_lineCount;                       // 0x3c0
    uint32_t m_pad3c4;
    uint32_t* m_lineStarts;                     // 0x3c8
    uint32_t m_lineCapacity;                    // 0x3d0
    uint32_t m_pad3d4;
    uint32_t m_lineGranularity;                 // 0x3d8
    uint32_t m_pad3dc;
    int32_t m_historyLines;                     // 0x3e0
    int32_t m_historyPos;                       // 0x3e4
    // TSFixedArray<EditHistory, 31>.
    uint32_t m_historyCount;                    // 0x3e8
    uint32_t m_pad3ec;
    EditHistory* m_history;                     // 0x3f0
    uint32_t m_historyCapacity;                 // 0x3f8
    uint32_t m_pad3fc;
    // The native listeners: enter, escape, space, tab, text changed, text set.
    struct LISTENER {
        uint32_t m_id;
        uint32_t m_pad;
        CSimpleEditBoxListener* m_listener;
    };
    LISTENER m_enterListener;                   // 0x400
    LISTENER m_escapeListener;                  // 0x410
    LISTENER m_spaceListener;                   // 0x420
    LISTENER m_tabListener;                     // 0x430
    LISTENER m_textChangedListener;             // 0x440
    LISTENER m_textSetListener;                 // 0x450
    bool m_imeComposing;                        // 0x460
    uint8_t m_pad461[3];
    int32_t m_inputLanguage;                    // 0x464
    CSimpleTexture* m_imeHighlight;             // 0x468
    int32_t m_imeStart;                         // 0x470
    int32_t m_imeEnd;                           // 0x474
    float m_textInsets[4];                      // 0x478 bottom, left, top, right

    // ref: FUN_1424af1c0
    explicit CSimpleEditBox(CSimpleFrame* parent);
    // ref: FUN_1424af6a0
    ~CSimpleEditBox() override;
    // Edit boxes come from their own pool (0x1447aedb0, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* editBox);

    // This class's type id (DAT_14309febc).
    static int32_t GetObjectType();
    // ref: FUN_1424b0730
    static void CreateScriptMetaTables();
    // ref: FUN_1424b1360
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d5e4a8, DAT_142d5e4ac).
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
    bool OnMouseMove(MOUSEEVENT* event) override;
    bool HasCharHandler() override;
    // Whether the IME may compose into it (not a password).
    bool Unknown120() override;
    bool HasKeyHandler(const void* event) override;
    bool OnChar(int32_t key) override;
    // An IME event (FUN_1424b45a0).
    bool Unknown150(void* event) override;
    bool OnKeyDown(void* event) override;
    bool OnKeyUp(void* event) override;
    // A key repeat runs the key down (FUN_1424b52d0).
    bool Unknown168(void* event) override;
    bool OnMouseDown(MOUSEEVENT* event, const char* button) override;
    bool OnMouseUp(MOUSEEVENT* event, const char* button) override;
    void QueueOnShow() override;
    void QueueOnHide() override;
    // Lets go of everything before the frame goes (FUN_1424b40a0).
    void Unknown280() override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void AddRegionsToBatch(CRenderBatch* batch) override;
    void OnSizeChanged(float width, float height) override;
    void OnEditFocusChanged(bool focused, bool runScripts, bool userInput) override;

    // ---- CLayoutFrame --------------------------------------------------------------------
    void ScaleBy(float* scale) override;
    void OnLayoutDepthChanged() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;

    // ---- CSimpleFontedFrame / CSimpleIMEHost ----------------------------------------------
    void FontObjectChanged(CSimpleFontedFrameFont* font, uint16_t flags) override;
    void PositionCandidates() override;
    float GetIMEFontHeight() override;
    float GetIMELayoutScale() override;

    // ---- focus ------------------------------------------------------------------------------
    // ref: FUN_1424b5fa0
    void SetFocus(bool userInput);
    // ref: FUN_1424afeb0
    void ClearFocus();
    // ref: FUN_1424b1c10
    bool HasFocus();
    // ref: FUN_1424b5cc0
    void SetEnabled(bool enabled);

    // ---- the text ---------------------------------------------------------------------------
    // The text, unless it was disabled by the security settings (FUN_1424b1b50).
    const char* GetText();
    // ref: FUN_1424b1c20
    bool HasText();
    // Replaces the text; `force` overrides EDIT_SECURITY_DISABLE_SET_TEXT, `always` runs the
    // scripts even for the same text (FUN_1424b64e0).
    void SetText(const char* text, const char* taint, bool force, bool always);
    // The script's SetText (FUN_1424b66d0).
    void SetTextScript(const char* text);
    // Inserts at the cursor over any highlight (FUN_1424b1db0): `runOnChar` runs OnChar,
    // `raw` skips the input mode checks and highlights what went in, `userInput` marks it as
    // typed, `force` overrides EDIT_SECURITY_DISABLE_SET_TEXT.
    void Insert(const char* text, const char* taint, bool runOnChar, bool raw, bool userInput, bool force);
    // The script's Insert (FUN_1424b2460).
    void InsertScript(const char* text);
    // A typed character (FUN_1424b1d10).
    void InsertChar(uint32_t code, bool force);
    // Removes [start, end) widened to whole tokens; true when anything went (FUN_1424b0fa0).
    bool DeleteRange(int32_t start, int32_t end, bool userInput);
    // ref: FUN_1424afee0
    void ClearText();
    // Re-sets the text through the input modes (FUN_1424b81b0).
    void ReapplyText();
    // ref: FUN_1424b19b0
    float GetNumber();
    // ref: FUN_1424b61d0
    void SetNumber(float number);
    // The letters (FUN_1424b1640); false when the text is hidden from scripts.
    bool GetDisplayText(char* buffer, int32_t capacity);
    // ref: FUN_1424b1940
    bool GetNumLetters(int32_t* count);
    // The letters among `count` bytes from `start` (backwards when negative) (FUN_1424b18a0).
    int32_t CountLetters(int32_t start, int32_t count);
    // Whether the text has outgrown a limit (FUN_1424af900).
    bool IsOverLimit();
    // Deletes from the end until the limits hold; false when it could not (FUN_1424b11c0).
    bool EnforceLimits(bool atEnd, bool userInput);

    // ---- the cursor and the highlight -----------------------------------------------------
    // ref: FUN_1424b5b90
    void SetCursorPosition(int32_t position);
    // The script's SetCursorPosition (FUN_1424b5c30).
    void SetCursorPositionScript(int32_t position);
    // ref: FUN_1424b1c40
    void HighlightText(int32_t start, int32_t end);
    // ref: FUN_1424afe00
    void ClearHighlight();
    // ref: FUN_1424b1410
    void ExtendHighlight(int32_t delta);
    // The first index at or after `index` that is not inside a texture code (FUN_1424b13b0).
    int32_t SkipHidden(int32_t index);
    // ref: FUN_1424b2e70
    void MoveCursor(int32_t count, bool extend);
    // ref: FUN_1424b30a0
    void MoveLeft(bool extend);
    // ref: FUN_1424b3600
    void MoveRight(bool extend);
    // ref: FUN_1424b3260
    void MoveWordLeft(bool extend);
    // ref: FUN_1424b3660
    void MoveLines(int32_t delta, bool extend);
    // ref: FUN_1424b39f0
    void MoveLineStart(bool extend);
    // ref: FUN_1424b3be0
    void MoveLineEnd(bool extend);
    // ref: FUN_1424b3d60
    void MoveToEnd(bool extend);
    // ref: FUN_1424b3ed0
    void MoveToStart(bool extend);
    // ref: FUN_1424b0780
    void DeleteChars(int32_t count, bool userInput);
    // ref: FUN_1424b0950
    void Backspace(bool userInput);
    // ref: FUN_1424b0b20
    void BackspaceWord(bool userInput);
    // ref: FUN_1424b0ec0
    void DeleteForward(bool userInput);
    // ref: FUN_1424b0f30
    void DeleteHighlight();
    // ref: FUN_1424b04d0
    bool CopyHighlight();
    // ref: FUN_1424b5730
    void Paste();

    // ---- history -----------------------------------------------------------------------------
    // ref: FUN_1424af7d0
    void AddHistoryLine(const char* text);
    // ref: FUN_1424afe20
    void ClearHistory();
    // ref: FUN_1424b5e60
    void SetHistoryLines(int32_t lines);
    // ref: FUN_1424af9f0
    void HistoryPrevious();
    // ref: FUN_1424b14d0
    void HistoryNext();

    // ---- modes -------------------------------------------------------------------------------
    // ref: FUN_1424b6020
    void SetMultiLine(bool multiLine);
    // ref: FUN_1424b62a0
    void SetNumeric(bool numeric);
    // ref: FUN_1424b62f0
    void SetNumericFullRange(bool numeric);
    // ref: FUN_1424b6330
    void SetPassword(bool password);
    // ref: FUN_1424b5820
    void SetAlphabeticOnly(bool enabled);
    // ref: FUN_1424b6370
    void SetSecureText(bool secure);
    // ref: FUN_1424b5810
    void ResetInputMode();
    // ref: FUN_1424b66f0
    void SetVisibleTextByteLimit(int32_t limit);

    // ---- the font ----------------------------------------------------------------------------
    // ref: FUN_1424b1810
    void GetFont(const char** path, float* height, uint8_t* flags);
    // ref: FUN_1424b5d60
    bool SetFont(const char* path, float height, const uint8_t* flags);
    // ref: FUN_1424b5dd0
    void SetFontObject(CSimpleFont* font);
    // ref: FUN_1424b5eb0
    void SetIndentedWordWrap(bool indented);
    // ref: FUN_1424b5f00
    void SetJustifyH(uint32_t justify);
    // ref: FUN_1424b5f50
    void SetJustifyV(uint32_t justify);
    // ref: FUN_1424b6620
    void SetTextColor(const uint32_t* rgb, const uint8_t* alpha);
    // ref: FUN_1424b63a0
    void SetShadowColor(const uint32_t* rgb, const uint8_t* alpha);
    // ref: FUN_1424b6430
    void SetShadowOffset(const float* offset);
    // ref: FUN_1424b6490
    void SetSpacing(float spacing);
    // ref: FUN_1424b5e20
    void SetHighlightColor(const uint32_t* rgb, const uint8_t* alpha);
    // ref: FUN_1424b66a0
    void SetTextInsets(float left, float right, float top, float bottom);
    // ref: FUN_1424b1b20
    void GetTextInsets(float* left, float* right, float* top, float* bottom);

    // ---- the IME -----------------------------------------------------------------------------
    // ref: FUN_1424b1860
    const char* GetInputLanguage();
    // ref: FUN_1424b67d0
    void ToggleInputLanguage();
    // ref: FUN_1424b67e0
    void UpdateImeComposition();
    // ref: FUN_1424b05d0
    void CreateImeHighlight();

    // ---- layout --------------------------------------------------------------------------------
    // Brings what changed up to date: lines, drag, scroll, highlight, cursor; OnTextChanged
    // when the text changed (FUN_1424b6c60).
    void Update(bool cursor);
    // The wrapped lines, when they changed (FUN_1424afa90).
    void UpdateLines();
    // The line a byte is on (FUN_1424b1a50).
    int32_t GetLineIndex(uint32_t position);
    // Scrolls a single line so `position` shows (FUN_1424b2b10).
    void ScrollTo(int32_t position, float offset, float width);
    // The byte under a point and whether it is left of, in or right of the text (FUN_1424aff80).
    bool GetIndexAtPoint(float x, float y, int32_t* index, int32_t* side);
    // The quad over [start, end) of a line (FUN_1424b6df0).
    void GetHighlightQuad(HighlightTextureInfo* quad, int32_t line, float lineHeight, int32_t start, int32_t end, bool local);
    // ref: FUN_1424b7190
    void UpdateDrag();
    // The font string inside the insets (FUN_1424b75d0).
    void UpdateSizes();
    // ref: FUN_1424b7790
    void UpdateCursor();
    // ref: FUN_1424b7b70
    void UpdateHighlight();
    // ref: FUN_1424b7e80
    void UpdateText();

    private:
    // Runs a native listener, when there is one.
    void Notify(LISTENER& listener);
    // The text the string shows (stars for a password).
    char* DisplayBuffer();
    // The highlight drops after an edit.
    void DropHighlight();
    // The cursor moved; blink restarts in the input phase it moved in.
    void CursorMoved();
};
static_assert(sizeof(CSimpleEditBox) == 0x488, "CSimpleEditBox is 0x488 bytes");

void CSimpleEditBox_FillScriptMethods(lua_State* L);

#endif
