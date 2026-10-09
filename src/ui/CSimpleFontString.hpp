#ifndef UI_C_SIMPLE_FONT_STRING_HPP
#define UI_C_SIMPLE_FONT_STRING_HPP

// CSimpleFontString (Engine/Source/Frame/CSimpleFontString.cpp): a region that draws text
// ("FontString", 0x2e8; vtable 0x1426cad48 with the CLayoutFrame vtable 0x1426cb020 at +0x18,
// the CAnimatableObject vtable 0x1426cb118 at +0x1d8 and the CSimpleFontable vtable 0x1426cb1b0
// at +0x218). It keeps the text, a font from the text block cache and the text block made
// from them, and rebuilds the block whenever the text, the font or the layout changes. |T and
// |A codes in the text become textures it owns; hyperlinks run the frame's handlers.

#include <cstdint>

#include "gxu/TextBlock.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleRegion.hpp"

class CSimpleTexture;
struct GXUEMBEDDEDTEXTURE;
struct uiBoundsRect;

// A texture an embedded |T or |A code draws with (0x18).
struct CSimpleEmbeddedTexture {
    CSimpleTexture* m_texture = nullptr;    // 0x00
    TSLink<CSimpleEmbeddedTexture> m_link;  // 0x08
};

// A font asked for before the string could take it (0x110), set when the layout is ready.
struct FONTSTRINGREQUEST {
    char m_path[0x104];             // 0x000
    int32_t m_fileDataId;           // 0x104 0 for a path
    uint8_t m_fontFlags;            // 0x108
    uint8_t m_pad109[3];
    float m_height;                 // 0x10c
};
static_assert(sizeof(FONTSTRINGREQUEST) == 0x110, "FONTSTRINGREQUEST is 0x110 bytes");

// A rect of text (uiBoundsRect, 0x10): left, bottom, width, height in UI units.
struct uiBoundsRect {
    float m_left;
    float m_bottom;
    float m_width;
    float m_height;
};

class CSimpleFontString : public CSimpleRegion, public CSimpleFontable {
    public:
    // m_stringFlags: the text block flags (TEXTBLOCK_*) and the font string's own.
    enum : uint32_t {
        FONTSTRING_JUSTIFY_H = 0x7,
        FONTSTRING_JUSTIFY_V = 0x38,
        FONTSTRING_JUSTIFY_TOP = 0x8,
        FONTSTRING_JUSTIFY_MIDDLE = 0x10,
        FONTSTRING_JUSTIFY_BOTTOM = 0x20,
        FONTSTRING_NO_WORD_WRAP = 0x40,
        FONTSTRING_SHADOW = 0x100,
        FONTSTRING_ONE_TO_ONE = 0x200,     // the font's own pixel height, not m_fontHeight
        FONTSTRING_NO_COLOR_CODES = 0x400,
        FONTSTRING_NON_SPACE_WRAP = 0x1000,
        FONTSTRING_INDENTED_WORD_WRAP = 0x20000,
    };

    // m_textFlags.
    enum : uint32_t {
        TEXT_FONTABLE = 0x1,               // takes settings from XML and font objects
        TEXT_FAMILY_CHECKED = 0x2,         // the family member was picked for this text
        TEXT_EXPANDED = 0x4,               // |K codes were expanded into s_expanded
        TEXT_PROCESSED = 0x8,              // m_textFlags & TEXT_EXPANDED is current
        TEXT_HYPERLINK_MASK = 0xffff0,     // the hyperlink under the mouse, 0xffff for none
    };

    FONTSTRINGREQUEST* m_fontRequest = nullptr;     // 0x248
    float m_rotation = 0.0f;                // 0x250
    float m_textScale = 1.0f;               // 0x254
    float m_baseRotation = 0.0f;            // 0x258 without the running animation
    float m_baseTextScale = 1.0f;           // 0x25c
    // The font strings waiting for a family member's file (0x142cce8f8, link offset 0x260).
    TSLink<CSimpleFontString> m_pendingLink;    // 0x260
    HTEXTFONT m_font;                       // 0x270
    float m_fontHeight = -1.0f;             // 0x280
    uint16_t m_maxBytes = 0;                // 0x284 0 for any length
    uint16_t m_textCapacity = 0;            // 0x286
    char* m_text = nullptr;                 // 0x288
    float m_spacing = 0.0f;                 // 0x290
    float m_fontScale = 1.0f;               // 0x294 the height's scale (SetTextScale)
    HTEXTBLOCK m_textBlock = nullptr;       // 0x298
    float m_stringWidth = 0.0f;             // 0x2a0 cached, 0 until measured
    float m_stringHeight = 0.0f;            // 0x2a4
    uint32_t m_shadowColor = 0xff000000;    // 0x2a8
    float m_shadowOffset[2] = {};           // 0x2ac
    int16_t m_gradientStart = -1;           // 0x2b4
    int16_t m_gradientLength = 0;           // 0x2b6
    uint32_t m_stringFlags = 0x212;         // 0x2b8
    // The m_stringFlags bits the font object still decides.
    uint32_t m_inheritedFlags = 0x2107f;    // 0x2bc
    uint32_t m_maxLines = 0;                // 0x2c0 low 24 bits
    uint32_t m_pad2c4 = 0;
    uint32_t m_textFlags = 0xffff1;         // 0x2c8
    uint32_t m_pad2cc = 0;
    TSExplicitList<CSimpleEmbeddedTexture> m_embeddedTextures;  // 0x2d0

    // The font strings alive (0x142e97bdc).
    static int64_t s_count;
    // The metatables (DAT_142cce8d0, DAT_142cce8d4).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;
    // Strings are batched even off the screen while this is set (DAT_142e97bd8).
    static uint8_t s_drawOffscreen;

    // ref: FUN_1406228b0
    CSimpleFontString(CSimpleFrame* frame, uint8_t drawLayer, bool show);
    // ref: FUN_140622a20
    ~CSimpleFontString() override;
    // Font strings come from their own pool (0x142e97be0).
    static void* operator new(size_t size);
    static void operator delete(void* string);

    // ref: FUN_140623c70
    static void CreateScriptMetaTables();
    // ref: FUN_140623cc0
    static void DestroyScriptMetaTables();
    // This class's type id (DAT_142e97c08).
    static int32_t GetObjectType();
    // The waiting font strings take their family member once its file is in (FUN_140624a60).
    static void OnFontFileLoaded(void* path, void* buffer, uint32_t size);
    // The font file request's buffer (FUN_140623360), and its error (0x1406233f0) and cancel
    // (0x1406233c0) ends, which free it.
    static void* AllocFontFileBuffer(uint32_t size);
    static void OnFontFileError(void* path, void* buffer, uint32_t size, const char* error);
    static void OnFontFileCanceled(void* path, void* buffer, uint32_t size);
    // The spare embedded texture nodes go (FUN_140624c80).
    static void DestroyEmbeddedTextureCache();

    // FrameScript_Object, CScriptObject and CScriptRegion.
    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void Unknown98(void* argument) override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void PostLoadXML(XMLOBJECT* node, CStatus* status) override;
    void OnMouseDown(MOUSEEVENT* event, const char* button) override;
    void OnMouseUp(MOUSEEVENT* event, const char* button) override;
    int32_t HitTest(int32_t type, MOUSEEVENT* event, bool force) override;
    void OnEnter(bool motion) override;
    void OnLeave(bool motion) override;

    // CSimpleRegion.
    void SetVertexColor(uint32_t rgb, const uint8_t* alpha) override;
    void UpdateEffectiveAlpha() override;
    void AddToBatch(CRenderBatch* batch) override;
    void SetDrawLayer(uint8_t layer, int8_t subLevel) override;
    uint64_t GetOwnDrawOrderKey() override;
    void Unknown2C0() override;

    // CLayoutFrame.
    void ScaleBy(float* scale) override;
    bool SetLayoutScale(float scale, bool force) override;
    void SetWidth(float width) override;
    void SetHeight(float height) override;
    float GetWidth() override;
    float GetHeight() override;
    bool IsLayoutReady() override;
    void OnLayoutReady() override;
    void OnLayoutDepthChanged() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;
    void ClearAnimTransform() override;

    // CAnimatableObject.
    void Unknown08() override;
    void Unknown40(void* value) override;
    void Unknown58(int32_t index, void* value, float radians) override;
    void Unknown60(int32_t index, void* first, void* second) override;
    void SetAnimAlpha(uint8_t alpha) override;

    // CSimpleFontable.
    void SetFontObject(CSimpleFont* font) override;
    void FontObjectUpdated(CSimpleFontAttributes* attributes) override;

    // ---- the font --------------------------------------------------------------------------
    // The font of a file at a height in UI units (FUN_140622410, by path FUN_140622660); false
    // when it cannot be made.
    bool SetFont(int32_t fileDataId, float height, uint8_t fontFlags, uint16_t attributeFlags);
    bool SetFont(const char* path, float height, uint8_t fontFlags, uint16_t attributeFlags);
    // The text block font flags (FUN_1406260b0).
    uint8_t GetFontFlags();
    int32_t GetFontFileDataId();            // FUN_1406251d0
    const char* GetFontName();              // FUN_140625250
    // The height text is laid out at: m_fontHeight, or the font's own (FUN_1406251f0).
    float GetFontHeight(bool oneToOne);
    // One line's height with the spacing, snapped to pixels (FUN_140623460), and scaled
    // (FUN_1406234d0).
    float GetLineHeight();
    float GetScaledLineHeight();
    // The font's real height for the string (FUN_140625870).
    float GetEffectiveFontHeight();
    // Sets the height without a new font (FUN_140628e70).
    void SetFontHeight(float height);
    // Sets m_fontScale (FUN_140628f80).
    void SetFontScale(float scale);

    // ---- text ------------------------------------------------------------------------------
    // Sets the text; `process` runs the localization grammar codes first (FUN_140628cc0).
    void SetText(const char* text, bool process);
    // The script's SetText: the font must be set (FUN_140628fe0).
    void SetTextChecked(const char* text);
    // The text to lay out, with |K codes expanded (FUN_140625c30).
    const char* GetDisplayText();
    // The display text cut to fit `width` by `height`, with "..." when it was (FUN_140624d40).
    const char* GetFittedText(float width, float height, bool* truncated);
    // Whether the fitted text was cut (FUN_140626d00).
    bool IsTruncated();
    // Caps the text at `bytes` (FUN_140628ec0).
    void SetMaxBytes(uint32_t bytes);
    // Lines and widths.
    uint32_t GetLineStarts(const char* text, float width, int32_t* offsets, uint32_t maxOffsets);  // FUN_140629b20
    uint32_t GetNumLines();                 // FUN_140625b80
    float GetStringWidth();                 // FUN_140625f10
    float GetStringHeight();                // FUN_140625d60
    float GetUnboundedStringWidth();        // FUN_140626520
    // The width of up to `length` bytes (FUN_1406260f0, of a range FUN_140623420).
    float GetTextWidth(const char* text, uint32_t length);
    float GetRangeWidth(const char* text, uint32_t start, uint32_t end);
    float GetWrappedWidth(const char* text);                                           // FUN_1406250a0
    uint32_t GetTextExtentCount(const char* text, uint32_t length, float width);      // FUN_140625920
    uint32_t GetTrailingCharCount(const char* text, uint32_t length, float width);    // FUN_140625a50
    // The width the layout gives it, or the measured one (FUN_1406264b0, FUN_1406252d0).
    float GetLayoutWidth();
    float GetLayoutHeight();

    // ---- appearance ------------------------------------------------------------------------
    void SetShadow(const uint32_t* color, const float* offset);    // FUN_140623100
    void ClearShadow();                                            // FUN_140628450
    void SetShadowColor(uint32_t* color, const uint8_t* alpha);    // FUN_140628b30
    void SetShadowOffset(const float* offset);                    // FUN_140628b80
    void SetSpacing(float spacing);                                // FUN_140628c10 (layout ready)
    void SetSpacingFromFont(float spacing);                        // FUN_140628c60
    void SetRotation(float radians);                               // FUN_140628b10
    bool SetAlphaGradient(int16_t start, int32_t length);          // FUN_1406285e0
    void SetJustifyH(uint32_t justify);
    void SetJustifyV(uint32_t justify);
    void SetStringFlag(uint32_t flag, bool on);

    // ---- the text block --------------------------------------------------------------------
    // Drops the text block and its sizes and asks for a new layout (FUN_140628480).
    void ClearString();
    // Rebuilds the text block now when the layout is ready (FUN_140629430).
    void UpdateString();
    // Picks the family member whose alphabet covers the text (FUN_140629910).
    void UpdateFamilyMember(const char* text, bool process, bool remember);
    // Puts the family back when a member stands in for it (FUN_140628500).
    void RestoreFamily();
    // Makes the embedded textures for the block's |T and |A codes (FUN_140628210), and lets them
    // go (FUN_140624b80).
    void CreateEmbeddedTextures();
    void ReleaseEmbeddedTextures();
    // Joins the list of strings waiting for a font file (FUN_1406231c0).
    void WaitForFontFile();

    // ---- hit testing -----------------------------------------------------------------------
    // The hyperlink at a point, with its rect (FUN_140624620).
    bool GetHyperlinkAt(const float* point, uint32_t* index, CRect* rect);
    // The character at a point (FUN_1406240a0).
    bool FindCharacterIndexAtCoordinate(float x, float y, int32_t* index, bool* inside);
    // The rects a span of characters covers (FUN_140623540).
    bool CalculateScreenAreaFromCharacterSpan(uint32_t start, uint32_t end, uiBoundsRect** rects, uint64_t* count);
};
static_assert(sizeof(CSimpleFontString) == 0x2e8, "CSimpleFontString is 0x2e8 bytes");

// Adds a font string's text block to a frame batch (FUN_140620970).
void RenderBatchAddFontString(CRenderBatch* batch, CSimpleFontString* string);
// The byte offset of the `count`th visible character (FUN_140624960).
int32_t FontStringCharCountToByteOffset(const char* text, int32_t count);
void CSimpleFontString_FillScriptMethods(lua_State* L);

#endif
