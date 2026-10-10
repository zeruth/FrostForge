#ifndef UI_C_SIMPLE_HTML_HPP
#define UI_C_SIMPLE_HTML_HPP

// Engine/Source/Frame/CSimpleHTML ("SimpleHTML", 0x358 bytes; vtable 0x14297d880, CLayoutFrame
// vtable 0x14297db98 at +0x18, CAnimatableObject vtable 0x14297dc90 at +0x1d8): text laid out
// top to bottom from a small HTML subset (<HTML><BODY> with H1, H2, H3, P, BR, IMG, HR and A),
// one font string per paragraph, styled by one font object per text type. Text that does not
// parse as HTML is shown as one plain paragraph.

#include <cstdint>

#include "blz/Vector.hpp"
#include "ui/CSimpleFrame.hpp"

class CSimpleFont;
class CSimpleRegion;
class CLayoutFrame;
class XMLNode;

// The tag of the visible element storage (0x142dd77b8).
extern const char g_visibleElementTag[];
// The tag of the text data storage (0x142d9c8c0).
extern const char g_htmlContentNodeTag[];

class CSimpleHTML : public CSimpleFrame {
    public:
    // The text types (the names at 0x142981cd0).
    enum : uint8_t {
        TEXTTYPE_P = 0,
        TEXTTYPE_H1 = 1,
        TEXTTYPE_H2 = 2,
        TEXTTYPE_H3 = 3,
        NUM_TEXTTYPES = 4,
    };
    // What an element shows.
    enum : int32_t {
        ELEMENT_TEXT = 1,
        ELEMENT_HYPERLINK_TEXT = 2,     // text with a |H link in it
        ELEMENT_TEXTURE = 3,
    };

    // A region the text made (0x20).
    struct VisibleElement {
        int32_t m_type;             // 0x00 ELEMENT_*
        uint32_t m_pad04;
        CSimpleRegion* m_region;    // 0x08
        uint8_t m_textType;         // 0x10
        bool m_hasTextType;         // 0x11
        uint8_t m_pad12[2];
        uint32_t m_justify;         // 0x14
        bool m_hasJustify;          // 0x18
        uint8_t m_pad19[7];
    };
    static_assert(sizeof(VisibleElement) == 0x20, "VisibleElement is 0x20 bytes");

    // A paragraph's text as GetTextData gives it (0x10).
    struct HTMLContentNode {
        const char* m_text;         // 0x00
        uint8_t m_textType;         // 0x08
        uint8_t m_pad09[3];
        uint32_t m_justify;         // 0x0c
    };
    static_assert(sizeof(HTMLContentNode) == 0x10, "HTMLContentNode is 0x10 bytes");

    blz::vector<VisibleElement, g_visibleElementTag> m_elements;    // 0x300
    // The last font string, the next element hangs below.
    CLayoutFrame* m_lastText;       // 0x318
    // The next element's offset below m_lastText (0 or less).
    float m_lineOffset;             // 0x320
    // Whether the frame's height follows the content.
    bool m_resizeToFit;             // 0x324
    uint8_t m_pad325[3];
    float m_contentHeight;          // 0x328
    // What is left of a floating image beside the text.
    float m_floatHeight;            // 0x32c
    CSimpleFont* m_fonts[NUM_TEXTTYPES];    // 0x330
    char* m_hyperlinkFormat;        // 0x350

    // ref: FUN_1424b86b0
    explicit CSimpleHTML(CSimpleFrame* parent);
    // ref: FUN_1424b8800
    ~CSimpleHTML() override;
    // SimpleHTML frames come from their own pool (0x1447aedd8).
    static void* operator new(size_t size);
    static void operator delete(void* html);

    // This class's type id (DAT_142e97b68).
    static int32_t GetObjectType();
    // ref: FUN_1424b8bd0
    static void CreateScriptMetaTables();
    // ref: FUN_1424b8c20
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d5e4f0, DAT_142d5e4f4).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    // The fonts and the elements go before the frame does.
    void Unknown280() override;

    // Lays the text out again; false when it was shown as plain text.
    // ref: FUN_1424ba6c0
    bool SetText(const char* text, CStatus* status, bool ignoreMarkup, const char* file);
    // The script's SetText, reporting through a status at the frame's location.
    // ref: FUN_1424ba940
    bool SetTextFromScript(const char* text, bool ignoreMarkup);
    // ref: FUN_1424ba460
    void SetHyperlinkFormat(const char* format);
    // ref: FUN_1424b8db0
    void GetTextData(blz::vector<HTMLContentNode, g_htmlContentNodeTag>* nodes);

    // ref: FUN_1424b8c70
    CSimpleFont* GetFontObject(uint8_t textType);
    // ref: FUN_1424ba400
    void SetFontObject(uint8_t textType, CSimpleFont* font);
    // ref: FUN_1424b8c80
    void GetFont(uint8_t textType, const char** path, float* height, uint8_t* flags);
    // ref: FUN_1424ba440
    void SetFont(uint8_t textType, const char* path, float height, const uint8_t* flags);
    // ref: FUN_1424b8cb0
    bool GetIndentedWordWrap(uint8_t textType);
    // ref: FUN_1424ba4b0
    void SetIndentedWordWrap(uint8_t textType, bool wrap);
    // ref: FUN_1424b8cd0
    uint32_t GetJustifyH(uint8_t textType);
    // ref: FUN_1424ba500
    void SetJustifyH(uint8_t textType, uint32_t justify);
    // ref: FUN_1424b8cf0
    uint32_t GetJustifyV(uint8_t textType);
    // ref: FUN_1424ba550
    void SetJustifyV(uint8_t textType, uint32_t justify);
    // ref: FUN_1424b8d60
    const uint32_t* GetShadowColor(uint8_t textType);
    // ref: FUN_1424ba5a0
    void SetShadowColor(uint8_t textType, uint32_t rgb, const uint8_t* alpha);
    // ref: FUN_1424b8d70
    const float* GetShadowOffset(uint8_t textType);
    // ref: FUN_1424ba610
    void SetShadowOffset(uint8_t textType, const float* offset);
    // ref: FUN_1424b8d80
    float GetSpacing(uint8_t textType);
    // ref: FUN_1424ba670
    void SetSpacing(uint8_t textType, float spacing);
    // ref: FUN_1424b8da0
    const uint32_t* GetTextColor(uint8_t textType);
    // ref: FUN_1424ba8e0
    void SetTextColor(uint8_t textType, uint32_t rgb, const uint8_t* alpha);

    private:
    // The children of BODY.
    // ref: FUN_1424b9350
    void AddBody(XMLNode* body, CStatus* status);
    // A heading or paragraph: its text with BR and A folded in, whitespace collapsed, in
    // pieces of at most 0x2000 bytes.
    // ref: FUN_1424b9e20
    void AddParagraph(XMLNode* node, uint8_t textType, CStatus* status);
    // A font string under the previous one. `justify` is {value, present}; it is filled from
    // the font when not given, and cleared once used.
    // ref: FUN_1424b88f0
    void AddText(const char* text, uint8_t textType, uint32_t* justify);
    // ref: FUN_1424b9900
    void AddImage(XMLNode* node, CStatus* status);
    // ref: FUN_1424b9500
    void AddRule(XMLNode* node, CStatus* status);
    // Counts an element's height into the content, past any floating image beside it.
    void AddContentHeight(float height);
    void PushElement(int32_t type, CSimpleRegion* region, const uint8_t* textType, const uint32_t* justify);
    const char* NameForStatus();
};
static_assert(sizeof(CSimpleHTML) == 0x358, "CSimpleHTML is 0x358 bytes");

void CSimpleHTML_FillScriptMethods(lua_State* L);

#endif
