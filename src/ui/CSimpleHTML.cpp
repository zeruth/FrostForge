#include "ui/CSimpleHTML.hpp"

#include <cstdlib>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/TextureAtlas.hpp"
#include "ui/UIScriptArgs.hpp"
#include "ui/UITypes.hpp"
#include "ui/XMLTree.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

// ui/CSimpleFontString.cpp
float SnapToPixels(float height);

const char g_visibleElementTag[] = ".?AUVisibleElement@CSimpleHTML@@";
const char g_htmlContentNodeTag[] = ".?AUHTMLContentNode@@";

int32_t CSimpleHTML::s_metaTable = -2;
int32_t CSimpleHTML::s_restrictedMetaTable = -2;

namespace {

// The SimpleHTML frames' pool (0x1447aedd8).
CDataAllocator s_htmlHeap(0x358, 5);

const char* const HTML_FILE = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleHTML.cpp";

// A rule's gap above and below, as fractions of the paragraph line (DAT_1426c7afc,
// DAT_142724c34).
constexpr float RULE_GAP_ABOVE = 0.2f;
constexpr float RULE_GAP_BELOW = 1.15f;

// The longest piece of a paragraph one font string takes.
constexpr uint64_t MAX_TEXT_PIECE = 0x2000;

bool IsSpace(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

// An attribute, or null when it is missing or empty.
const char* NonEmptyAttribute(XMLNode* node, const char* name) {
    const char* value = node->GetAttribute(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

// A length attribute in UI units, 0 when missing.
float LengthAttribute(XMLNode* node, const char* name) {
    const char* value = NonEmptyAttribute(node, name);
    return value != nullptr ? ScriptMethod::ToLayoutUnits(static_cast<double>(SStrToFloat(value))) : 0.0f;
}

// Copies up to `count` bytes, stopping after a NUL; a copy that ran the full count is
// terminated (the reference's strncpy that always terminates).
void CopyPrefix(char* dest, const char* source, int64_t count) {
    if (count == -1) {
        return;
    }
    for (; count != 0; count--) {
        char c = *source++;
        *dest = c;
        if (c == '\0') {
            return;
        }
        dest++;
    }
    *dest = '\0';
}

// A font's settings changed through the HTML frame: mark them set and tell its fontables.
void FontChanged(CSimpleFont* font, uint16_t has, uint16_t changed) {
    font->m_attributes.m_flags |= has | changed;
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424b86b0
CSimpleHTML::CSimpleHTML(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_lastText = nullptr;
    this->m_lineOffset = 0.0f;
    this->m_resizeToFit = false;
    this->m_contentHeight = 0.0f;
    this->m_floatHeight = 0.0f;
    for (CSimpleFont*& font : this->m_fonts) {
        font = new (0x1f, HTML_FILE, SStrHashFnv1a(HTML_FILE), 0x15) CSimpleFont();
    }
    this->m_hyperlinkFormat = nullptr;
    SMemFree(nullptr, HTML_FILE, 0x109, 0);
    this->m_hyperlinkFormat = SStrDupA("|H%s|h%s|h", nullptr, 0);
    this->SetHyperlinksEnabled(true);
    this->m_resizeToFit = false;
}

// ref: FUN_1424b8800
CSimpleHTML::~CSimpleHTML() = default;

void* CSimpleHTML::operator new(size_t size) {
    (void)size;
    return s_htmlHeap.GetData(false, ".?AVCSimpleHTML@@", -2, 0x1f);
}

void CSimpleHTML::operator delete(void* html) {
    if (html != nullptr) {
        s_htmlHeap.PutData(html);
    }
}

// DAT_142e97b68
int32_t CSimpleHTML::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424b8bd0
void CSimpleHTML::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleHTML_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleHTML_FillScriptMethods, true);
}

// ref: FUN_1424b8c20
void CSimpleHTML::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ref: FUN_1424b92a0
void CSimpleHTML::Unknown280() {
    for (CSimpleFont* font : this->m_fonts) {
        delete font;
    }
    for (VisibleElement& element : this->m_elements) {
        delete element.m_region;
    }
    SMemFree(this->m_hyperlinkFormat, HTML_FILE, 0x2a, 0);
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424b8d30
int32_t CSimpleHTML::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424b9020
bool CSimpleHTML::IsObjectTypeName(const char* name) {
    return _stricmp(name, "SimpleHTML") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424b8f20
bool CSimpleHTML::IsObjectType(int32_t type) {
    if (type == CSimpleHTML::GetObjectType() || type == CSimpleFrame::GetObjectType()) {
        return true;
    }
    return this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_1424b8d20
const char* CSimpleHTML::GetObjectTypeName() {
    return "SimpleHTML";
}

// ref: FUN_1424b8d10
uint32_t CSimpleHTML::GetObjectSize() {
    return 0x358;
}

const char* CSimpleHTML::NameForStatus() {
    const char* name = this->GetName();
    return name != nullptr ? name : this->GetObjectTypeName();
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424b90a0
void CSimpleHTML::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x40000)) {
        return;
    }
    auto xml = static_cast<XMLSIMPLEHTML*>(node);
    if (xml->m_fontFlags & 0x80) {
        const char* fontName = xml->m_font.m_text;
        CSimpleFont* font = CSimpleFont::GetFont(fontName, false);
        if (font == nullptr) {
            status->Add(1, "%s %s: Couldn't find font named %s", this->GetObjectTypeName(), this->NameForStatus(), fontName);
        } else {
            for (CSimpleFont* textFont : this->m_fonts) {
                textFont->CSimpleFontable::SetFontObject(font);
            }
        }
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        uint32_t type = child->m_type;
        if (type == XMLTYPE_FONTSTRING) {
            this->m_fonts[TEXTTYPE_P]->LoadXML(child, status);
        } else if (type == XMLTYPE_FONTSTRINGHEADER1) {
            this->m_fonts[TEXTTYPE_H1]->LoadXML(child, status);
        } else if (type == XMLTYPE_FONTSTRINGHEADER2) {
            this->m_fonts[TEXTTYPE_H2]->LoadXML(child, status);
        } else if (type == XMLTYPE_FONTSTRINGHEADER3) {
            this->m_fonts[TEXTTYPE_H3]->LoadXML(child, status);
        }
    }
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if ((xml->m_htmlFlags & 4) && cas::CASFileLoadByName(xml->m_file.m_text, &data, &size, 3, 0x20)) {
        this->SetText(reinterpret_cast<const char*>(data), status, false, xml->m_file.m_text);
        cas::CASFileFree(data);
    }
    if (xml->m_htmlFlags & 8) {
        const char* format = xml->m_hyperlinkFormat.m_text;
        SMemFree(this->m_hyperlinkFormat, HTML_FILE, 0x109, 0);
        this->m_hyperlinkFormat = SStrDupA(format, nullptr, 0);
    }
    if (xml->m_htmlFlags & 1) {
        this->m_resizeToFit = ((xml->m_htmlFlags >> 1) & 1) != 0;
    }
}

// ---- the text ---------------------------------------------------------------------------------

// ref: FUN_1424ba6c0
bool CSimpleHTML::SetText(const char* text, CStatus* status, bool ignoreMarkup, const char* file) {
    const char* source = text != nullptr ? text : "";
    for (VisibleElement& element : this->m_elements) {
        CSimpleRegion* region = element.m_region;
        if (region->m_regionFlags & CSimpleRegion::REGIONFLAG_ITERATING) {
            CSimpleRegion::DeferDelete(region);
        } else {
            delete region;
        }
    }
    this->m_elements.clear();
    this->m_lastText = nullptr;
    this->m_lineOffset = 0.0f;
    this->m_contentHeight = 0.0f;
    this->m_floatHeight = 0.0f;
    if (!ignoreMarkup) {
        XMLTREE* tree = XMLTree_Load(source, static_cast<uint32_t>(std::strlen(source)), status, file);
        if (tree != nullptr) {
            XMLNode* root = XMLTree_GetRoot(tree);
            if (_stricmp(root->m_name.m_data, "HTML") == 0) {
                bool parsed = false;
                for (XMLNode* child = root->m_child; child != nullptr; child = child->m_next) {
                    if (_stricmp(child->m_name.m_data, "BODY") == 0) {
                        parsed = true;
                        this->AddBody(child, status);
                        break;
                    }
                    status->Add(1, "Frame %s: Unknown element type: %s (expected BODY)", this->NameForStatus(), child->m_name.m_data);
                }
                XMLTree_Free(tree);
                if (parsed) {
                    return true;
                }
            } else {
                status->Add(1, "Frame %s: Unknown element type: %s (expected HTML)", this->NameForStatus(), root->m_name.m_data);
                XMLTree_Free(tree);
            }
        }
    }
    uint32_t justify[2] = { 0, 0 };
    this->AddText(source, TEXTTYPE_P, justify);
    return false;
}

// ref: FUN_1424ba940
bool CSimpleHTML::SetTextFromScript(const char* text, bool ignoreMarkup) {
    CScriptStatus status;
    status.m_warn = true;
    XMLStatusRestoreLocation(&status, this->GetScriptLocation());
    status.m_warn = false;
    return this->SetText(text, &status, ignoreMarkup, nullptr);
}

// ref: FUN_1424b9350
void CSimpleHTML::AddBody(XMLNode* body, CStatus* status) {
    for (XMLNode* child = body->m_child; child != nullptr; child = child->m_next) {
        const char* name = child->m_name.m_data;
        if (_stricmp(name, "H1") == 0) {
            this->AddParagraph(child, TEXTTYPE_H1, status);
        } else if (_stricmp(name, "H2") == 0) {
            this->AddParagraph(child, TEXTTYPE_H2, status);
        } else if (_stricmp(name, "H3") == 0) {
            this->AddParagraph(child, TEXTTYPE_H3, status);
        } else if (_stricmp(name, "P") == 0) {
            this->AddParagraph(child, TEXTTYPE_P, status);
        } else if (_stricmp(name, "BR") == 0) {
            uint32_t justify[2] = { 0, 0 };
            this->AddText("\n", TEXTTYPE_P, justify);
        } else if (_stricmp(name, "IMG") == 0) {
            this->AddImage(child, status);
        } else if (_stricmp(name, "HR") == 0) {
            this->AddRule(child, status);
        } else {
            status->Add(1, "Frame %s: Unknown element type: %s", this->NameForStatus(), name);
        }
    }
}

// ref: FUN_1424b9e20
void CSimpleHTML::AddParagraph(XMLNode* node, uint8_t textType, CStatus* status) {
    uint32_t justify = 1;
    if (const char* align = NonEmptyAttribute(node, "align")) {
        StringToJustify(align, &justify);
    }
    char* text = SStrDupA(node->m_body != nullptr ? node->m_body : "", nullptr, 0);
    // How far the text has grown past the offsets the parser recorded.
    int64_t shift = 0;
    for (XMLNode* child = node->m_child; child != nullptr; child = child->m_next) {
        const char* name = child->m_name.m_data;
        if (_stricmp(name, "BR") == 0) {
            uint64_t length = std::strlen(text);
            auto joined = static_cast<char*>(SMemAlloc(length + 3, 0x1f, HTML_FILE, SStrHashFnv1a(HTML_FILE), 0x17d, 0));
            int64_t at = child->m_bodyOffset + shift;
            CopyPrefix(joined, text, at);
            std::strcat(joined, "|n");
            std::strcat(joined, text + child->m_bodyOffset + shift);
            SMemFree(text, HTML_FILE, 0x181, 0);
            text = joined;
            shift += 2;
        } else if (_stricmp(name, "A") == 0) {
            const char* body = child->m_body;
            const char* href = child->GetAttribute("href");
            if (body == nullptr || *body == '\0' || href == nullptr || *href == '\0') {
                continue;
            }
            int64_t linkSize = static_cast<int64_t>(std::strlen(body) + 1 + std::strlen(this->m_hyperlinkFormat) + std::strlen(href));
            int64_t length = static_cast<int64_t>(std::strlen(text));
            auto joined = static_cast<char*>(SMemAlloc(length + linkSize * 2, 0x1f, HTML_FILE, SStrHashFnv1a(HTML_FILE), 0x18c, 0));
            char* link = joined + linkSize + length;
            SStrPrintf(link, static_cast<uint64_t>(linkSize), this->m_hyperlinkFormat, href, body);
            int64_t at = child->m_bodyOffset + shift;
            CopyPrefix(joined, text, at);
            std::strcat(joined, link);
            std::strcat(joined, text + child->m_bodyOffset + shift);
            SMemFree(text, HTML_FILE, 0x192, 0);
            text = joined;
            shift += static_cast<int64_t>(std::strlen(link));
        } else {
            status->Add(1, "Frame %s: Unknown element type: %s", this->NameForStatus(), name);
        }
    }

    // Runs of whitespace become one space, none at the start of a line or before a |n.
    char* out = text;
    if (*text != '\0') {
        bool lineStart = true;
        bool spaced = false;
        for (const char* in = text; *in != '\0'; in++) {
            char c = *in;
            if (IsSpace(c)) {
                if (!lineStart && !spaced) {
                    spaced = true;
                    *out++ = ' ';
                }
            } else if (c == '|' && in[1] == 'n') {
                if (text < out && spaced) {
                    out--;
                }
                out[0] = '|';
                out[1] = 'n';
                out += 2;
                spaced = false;
                lineStart = true;
                in++;
            } else {
                lineStart = false;
                *out++ = c;
                spaced = false;
            }
        }
        if (text < out && spaced) {
            out--;
        }
    }
    *out = '\0';

    // A font string takes at most 0x2000 bytes: longer text breaks at its last |n, else its
    // last space.
    char piece[MAX_TEXT_PIECE];
    const char* rest = text;
    while (true) {
        uint64_t length = std::strlen(rest);
        if (length <= MAX_TEXT_PIECE) {
            uint32_t align[2] = { justify, 1 };
            this->AddText(rest, textType, align);
            SMemFree(text, HTML_FILE, 0x1ea, 0);
            return;
        }
        int64_t cut = MAX_TEXT_PIECE - 1;
        while (!(rest[cut] == '|' && rest[cut + 1] == 'n')) {
            cut--;
            if (cut <= 0) {
                break;
            }
        }
        int64_t skip;
        if (cut <= 0) {
            cut = MAX_TEXT_PIECE - 1;
            while (rest[cut] != ' ') {
                cut--;
                if (cut <= 0) {
                    break;
                }
            }
            if (cut < 0) {
                cut = 0;
            }
            std::memcpy(piece, rest, static_cast<size_t>(cut));
            skip = cut + 1;
        } else {
            std::memcpy(piece, rest, static_cast<size_t>(cut));
            skip = cut + 2;
        }
        rest += skip;
        piece[cut] = '\0';
        uint32_t align[2] = { justify, 1 };
        this->AddText(piece, textType, align);
    }
}

void CSimpleHTML::AddContentHeight(float height) {
    float floating = this->m_floatHeight;
    if (0.0f < floating) {
        if (height < floating) {
            this->m_floatHeight = floating - height;
            return;
        }
        height -= floating;
        this->m_floatHeight = 0.0f;
    }
    this->m_contentHeight += height;
    if (this->m_resizeToFit && 0.0f < height) {
        this->SetHeight(this->m_contentHeight);
    }
}

void CSimpleHTML::PushElement(int32_t type, CSimpleRegion* region, const uint8_t* textType, const uint32_t* justify) {
    VisibleElement element = {};
    element.m_type = type;
    element.m_region = region;
    if (textType != nullptr) {
        element.m_textType = *textType;
        element.m_hasTextType = true;
    }
    if (justify != nullptr) {
        element.m_justify = *justify;
        element.m_hasJustify = true;
    }
    this->m_elements.push_back(element);
}

// ref: FUN_1424b88f0
void CSimpleHTML::AddText(const char* text, uint8_t textType, uint32_t* justify) {
    auto string = new CSimpleFontString(this, DRAWLAYER_ARTWORK, true);
    if (this->m_lastText == nullptr) {
        string->SetPoint(FRAMEPOINT_TOPLEFT, this, FRAMEPOINT_TOPLEFT, 0.0f, 0.0f, true);
    } else {
        string->SetPoint(FRAMEPOINT_TOPLEFT, this->m_lastText, FRAMEPOINT_BOTTOMLEFT, 0.0f, this->m_lineOffset, true);
    }
    string->SetWidth(this->GetWidth());
    CSimpleFont* font = this->m_fonts[textType];
    if (font->m_attributes.m_fontPath == nullptr) {
        font = this->m_fonts[TEXTTYPE_P];
    }
    string->CSimpleFontable::SetFontObject(font);
    bool hasJustify = justify[1] != 0;
    if (!hasJustify) {
        uint32_t fontJustify = font->m_attributes.m_stringFlags & CSimpleFontString::FONTSTRING_JUSTIFY_H;
        justify[0] = fontJustify != 0 ? fontJustify : 1;
        justify[1] = 1;
    }
    // The string keeps its own justification and wrapping from now on.
    string->m_inheritedFlags &= ~CSimpleFontString::FONTSTRING_JUSTIFY_H;
    string->SetJustifyH(justify[0]);
    string->m_inheritedFlags &= ~CSimpleFontString::FONTSTRING_NON_SPACE_WRAP;
    string->SetStringFlag(CSimpleFontString::FONTSTRING_NON_SPACE_WRAP, true);
    string->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    string->SetText(text, true);
    this->m_lastText = string;
    this->m_lineOffset = -SnapToPixels(string->m_spacing);
    float height = string->GetStringHeight() - this->m_lineOffset;
    this->AddContentHeight(height);
    bool hyperlink = std::strstr(text, "|H") != nullptr;
    uint8_t type = textType;
    this->PushElement(hyperlink ? ELEMENT_HYPERLINK_TEXT : ELEMENT_TEXT, string, &type, justify[1] != 0 ? justify : nullptr);
    justify[1] = 0;
}

// ref: FUN_1424b9500
void CSimpleHTML::AddRule(XMLNode* node, CStatus* status) {
    (void)status;
    uint32_t align = 2;
    if (const char* value = NonEmptyAttribute(node, "align")) {
        StringToJustify(value, &align);
    }
    float width = LengthAttribute(node, "width");
    auto rule = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
    TEXTUREATLASSCALE scale = { CSimpleTop::s_instance->GetRegionLayoutVersion(rule), 1 };
    int32_t member = 0;
    TextureAtlasFindByName(&member, "Book-line", &scale);
    if (member != 0 && rule->SetAtlas(&member, 1, false, 0, 0)) {
        float size[2] = {};
        TextureAtlasGetSize(&member, size);
        if (width <= 0.0f) {
            width = ScriptMethod::ToLayoutUnits(static_cast<double>(size[0]));
        }
        rule->SetSize(width, ScriptMethod::ToLayoutUnits(static_cast<double>(size[1])));
    }
    float previous = this->m_lineOffset;
    CSimpleFont* font = this->m_fonts[TEXTTYPE_P];
    float line = 0.0f;
    if (font != nullptr) {
        line = font->m_attributes.m_spacing + font->m_attributes.m_height;
        if (0.0f < line) {
            this->m_lineOffset -= SnapToPixels(line * RULE_GAP_ABOVE);
        }
    }
    CLayoutFrame* below = this->m_lastText;
    float y = below != nullptr ? this->m_lineOffset : 0.0f;
    if (align == 1) {
        rule->SetPoint(FRAMEPOINT_TOPLEFT, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOMLEFT : FRAMEPOINT_TOPLEFT, 0.0f, y, true);
    } else if (align == 2) {
        rule->SetPoint(FRAMEPOINT_TOP, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOM : FRAMEPOINT_TOP, 0.0f, y, true);
    } else if (align == 4) {
        rule->SetPoint(FRAMEPOINT_TOPRIGHT, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOMRIGHT : FRAMEPOINT_TOPRIGHT, 0.0f, y, true);
    }
    this->m_lineOffset -= SnapToPixels(rule->GetHeight());
    if (0.0f < line) {
        this->m_lineOffset -= SnapToPixels(line * RULE_GAP_BELOW);
    }
    this->AddContentHeight(previous - this->m_lineOffset);
    this->PushElement(ELEMENT_TEXTURE, rule, nullptr, nullptr);
}

// ref: FUN_1424b9900
void CSimpleHTML::AddImage(XMLNode* node, CStatus* status) {
    (void)status;
    uint32_t align = 1;
    bool floating = false;
    if (const char* value = NonEmptyAttribute(node, "align")) {
        StringToJustify(value, &align);
        floating = align == 1 || align == 4;
    }
    float width = LengthAttribute(node, "width");
    float height = LengthAttribute(node, "height");
    float x = LengthAttribute(node, "XOffset");
    float y = LengthAttribute(node, "YOffset");
    // top, left, bottom, right
    float texCoords[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (const char* value = NonEmptyAttribute(node, "texCoordLeft")) {
        texCoords[1] = SStrToFloat(value);
    }
    if (const char* value = NonEmptyAttribute(node, "texCoordTop")) {
        texCoords[0] = SStrToFloat(value);
    }
    if (const char* value = NonEmptyAttribute(node, "texCoordRight")) {
        texCoords[3] = SStrToFloat(value);
    }
    if (const char* value = NonEmptyAttribute(node, "texCoordBottom")) {
        texCoords[2] = SStrToFloat(value);
    }
    auto image = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
    image->SetWidth(width);
    image->SetHeight(height);
    CLayoutFrame* below = this->m_lastText;
    float offsetY = below != nullptr ? y + this->m_lineOffset : y;
    if (align == 1) {
        image->SetPoint(FRAMEPOINT_TOPLEFT, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOMLEFT : FRAMEPOINT_TOPLEFT, x, offsetY, true);
    } else if (align == 2) {
        image->SetPoint(FRAMEPOINT_TOP, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOM : FRAMEPOINT_TOP, x, offsetY, true);
    } else if (align == 4) {
        image->SetPoint(FRAMEPOINT_TOPRIGHT, below != nullptr ? below : this, below != nullptr ? FRAMEPOINT_BOTTOMRIGHT : FRAMEPOINT_TOPRIGHT, x, offsetY, true);
    }
    image->SetTextureByName(node->GetAttribute("src"), 0, 0, 1, true, false);
    image->SetTexCoordsRect(texCoords);
    float imageHeight = image->GetHeight() - y;
    if (imageHeight <= 0.0f) {
        imageHeight = 0.0f;
    }
    if (!floating) {
        this->m_lineOffset -= imageHeight;
    }
    this->AddContentHeight(imageHeight);
    if (floating) {
        this->m_floatHeight = imageHeight;
    }
    this->PushElement(ELEMENT_TEXTURE, image, nullptr, nullptr);
}

// ref: FUN_1424ba460
void CSimpleHTML::SetHyperlinkFormat(const char* format) {
    SMemFree(this->m_hyperlinkFormat, HTML_FILE, 0x109, 0);
    this->m_hyperlinkFormat = SStrDupA(format, nullptr, 0);
}

// ref: FUN_1424b8db0
void CSimpleHTML::GetTextData(blz::vector<HTMLContentNode, g_htmlContentNodeTag>* nodes) {
    for (VisibleElement& element : this->m_elements) {
        if ((element.m_type != ELEMENT_TEXT && element.m_type != ELEMENT_HYPERLINK_TEXT) || element.m_region == nullptr
            || !element.m_region->IsObjectType(CSimpleFontString::GetObjectType())) {
            continue;
        }
        HTMLContentNode content = {};
        const char* text = static_cast<CSimpleFontString*>(element.m_region)->m_text;
        content.m_text = text != nullptr && *text != '\0' ? text : nullptr;
        content.m_textType = element.m_hasTextType ? element.m_textType : 0;
        content.m_justify = element.m_hasJustify ? element.m_justify : 1;
        nodes->push_back(content);
    }
}

// ---- the text type fonts ------------------------------------------------------------------------

CSimpleFont* CSimpleHTML::GetFontObject(uint8_t textType) {
    return this->m_fonts[textType];
}

void CSimpleHTML::SetFontObject(uint8_t textType, CSimpleFont* font) {
    CSimpleFont* textFont = this->m_fonts[textType];
    for (CSimpleFont* check = font; check != nullptr; check = check->m_fontObject) {
        if (check == textFont) {
            FrameScript_ActionError("Can't create a font object loop");
            return;
        }
    }
    textFont->CSimpleFontable::SetFontObject(font);
}

void CSimpleHTML::GetFont(uint8_t textType, const char** path, float* height, uint8_t* flags) {
    this->m_fonts[textType]->GetFontInfo(path, height, flags);
}

void CSimpleHTML::SetFont(uint8_t textType, const char* path, float height, const uint8_t* flags) {
    FontSetFont(this->m_fonts[textType], path, height, flags);
}

bool CSimpleHTML::GetIndentedWordWrap(uint8_t textType) {
    return ((this->m_fonts[textType]->m_attributes.m_stringFlags >> 0x11) & 1) != 0;
}

void CSimpleHTML::SetIndentedWordWrap(uint8_t textType, bool wrap) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    uint32_t flags = wrap ? (attributes.m_stringFlags | 0x20000) : (attributes.m_stringFlags & ~0x20000u);
    if (flags != attributes.m_stringFlags || !(attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS)) {
        attributes.m_stringFlags = flags;
        FontChanged(font, CSimpleFontAttributes::HAS_FLAGS, CSimpleFontAttributes::CHANGED_FLAGS);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    font->UpdateFontables();
}

uint32_t CSimpleHTML::GetJustifyH(uint8_t textType) {
    return this->m_fonts[textType]->m_attributes.m_stringFlags & 7;
}

void CSimpleHTML::SetJustifyH(uint8_t textType, uint32_t justify) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    uint32_t flags = (attributes.m_stringFlags & ~7u) | (justify & 7);
    if (flags != attributes.m_stringFlags || !(attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS)) {
        attributes.m_stringFlags = flags;
        FontChanged(font, CSimpleFontAttributes::HAS_FLAGS, CSimpleFontAttributes::CHANGED_FLAGS);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    font->UpdateFontables();
}

uint32_t CSimpleHTML::GetJustifyV(uint8_t textType) {
    return this->m_fonts[textType]->m_attributes.m_stringFlags & 0x38;
}

void CSimpleHTML::SetJustifyV(uint8_t textType, uint32_t justify) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    uint32_t flags = (attributes.m_stringFlags & ~0x38u) | (justify & 0x38);
    if (flags != attributes.m_stringFlags || !(attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS)) {
        attributes.m_stringFlags = flags;
        FontChanged(font, CSimpleFontAttributes::HAS_FLAGS, CSimpleFontAttributes::CHANGED_FLAGS);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    font->UpdateFontables();
}

const uint32_t* CSimpleHTML::GetShadowColor(uint8_t textType) {
    return &this->m_fonts[textType]->m_attributes.m_shadowColor;
}

void CSimpleHTML::SetShadowColor(uint8_t textType, uint32_t rgb, const uint8_t* alpha) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    uint32_t color = (rgb & 0xffffff) | (static_cast<uint32_t>(alpha[1] != 0 ? alpha[0] : 0xff) << 24);
    if (color != attributes.m_shadowColor || !(attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW)) {
        attributes.m_shadowColor = color;
        FontChanged(font, CSimpleFontAttributes::HAS_SHADOW, CSimpleFontAttributes::CHANGED_SHADOW);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
    font->UpdateFontables();
}

const float* CSimpleHTML::GetShadowOffset(uint8_t textType) {
    return this->m_fonts[textType]->m_attributes.m_shadowOffset;
}

void CSimpleHTML::SetShadowOffset(uint8_t textType, const float* offset) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (attributes.m_shadowOffset[0] != offset[0] || attributes.m_shadowOffset[1] != offset[1]
        || !(attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW)) {
        attributes.m_shadowOffset[0] = offset[0];
        attributes.m_shadowOffset[1] = offset[1];
        FontChanged(font, CSimpleFontAttributes::HAS_SHADOW, CSimpleFontAttributes::CHANGED_SHADOW);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
    font->UpdateFontables();
}

float CSimpleHTML::GetSpacing(uint8_t textType) {
    return this->m_fonts[textType]->m_attributes.m_spacing;
}

void CSimpleHTML::SetSpacing(uint8_t textType, float spacing) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    float delta = spacing - attributes.m_spacing;
    // DAT_14261a954
    if (2.3841858e-07f <= (delta < 0.0f ? -delta : delta) || !(attributes.m_flags & CSimpleFontAttributes::HAS_SPACING)) {
        attributes.m_spacing = spacing;
        FontChanged(font, CSimpleFontAttributes::HAS_SPACING, CSimpleFontAttributes::CHANGED_SPACING);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SPACING;
    font->UpdateFontables();
}

const uint32_t* CSimpleHTML::GetTextColor(uint8_t textType) {
    return &this->m_fonts[textType]->m_attributes.m_color;
}

void CSimpleHTML::SetTextColor(uint8_t textType, uint32_t rgb, const uint8_t* alpha) {
    CSimpleFont* font = this->m_fonts[textType];
    CSimpleFontAttributes& attributes = font->m_attributes;
    uint32_t color = (rgb & 0xffffff) | (static_cast<uint32_t>(alpha[1] != 0 ? alpha[0] : 0xff) << 24);
    if (color != attributes.m_color || !(attributes.m_flags & CSimpleFontAttributes::HAS_COLOR)) {
        attributes.m_color = color;
        FontChanged(font, CSimpleFontAttributes::HAS_COLOR, CSimpleFontAttributes::CHANGED_COLOR);
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
    font->UpdateFontables();
}

// ---- script methods (table 0x142cf8c30) ------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleHTML* HtmlSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleHTML*>(Self(L, CSimpleHTML::GetObjectType(), index, usage));
}

struct TEXTTYPENAME {
    uint8_t m_value;
    const char* m_name;
};

// The text types by name (0x142981cd0).
const TEXTTYPENAME s_textTypes[] = {
    { CSimpleHTML::TEXTTYPE_P, "P" },
    { CSimpleHTML::TEXTTYPE_H1, "H1" },
    { CSimpleHTML::TEXTTYPE_H2, "H2" },
    { CSimpleHTML::TEXTTYPE_H3, "H3" },
};

// ref: FUN_1424df360
bool StringToTextType(const char* string, uint8_t* type) {
    for (const auto& entry : s_textTypes) {
        if (_stricmp(entry.m_name, string) == 0) {
            *type = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_1424df330
const char* TextTypeToString(uint8_t type) {
    for (const auto& entry : s_textTypes) {
        if (entry.m_value == type) {
            return entry.m_name;
        }
    }
    return nullptr;
}

// A required text type name (FUN_1408c5d70).
uint8_t RequiredTextType(lua_State* L, int32_t* index, const char* usage) {
    uint8_t type = 0;
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0;
    }
    if (lua_type(L, *index) != LUA_TSTRING || !StringToTextType(lua_tolstring(L, *index, nullptr), &type)) {
        luaL_argerror(L, *index, usage);
        return 0;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    (*index)++;
    return type;
}

// A required string (lua_isstring), or the usage error.
const char* RequiredText(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return nullptr;
    }
    if (!lua_isstring(L, *index)) {
        luaL_argerror(L, *index, usage);
        return nullptr;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return nullptr;
    }
    const char* text = lua_tolstring(L, *index, nullptr);
    (*index)++;
    return text;
}

// A font object, or a font's name (FUN_140a508c0).
CSimpleFont* FontArgument(lua_State* L, int32_t index) {
    int32_t type = lua_type(L, index);
    if (type == LUA_TSTRING) {
        return CSimpleFont::GetFont(lua_tolstring(L, index, nullptr), false);
    }
    if (type == LUA_TTABLE) {
        const char* error = nullptr;
        return static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), index, false, false));
    }
    return nullptr;
}

// ref: FUN_1409d1c90
int GetContentHeight(lua_State* L) {
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, "Usage: local height = self:GetContentHeight()");
    // FUN_1413558c0
    float height = html->m_contentHeight;
    RaisePendingError(L, html, "GetContentHeight");
    return PushUIUnits(L, height);
}

// ref: FUN_1409d2a10
int GetFont(lua_State* L) {
    const char* usage = "Usage: local fontFile, height, flags = self:GetFont(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    const char* path = "";
    float height = 0.0f;
    uint8_t flags = 0;
    html->GetFont(type, &path, &height, &flags);
    RaisePendingError(L, html, "GetFont");
    PushStringOrNil(L, path);
    PushUIUnits(L, height);
    // FUN_1408c6dd0
    PushStringOrNil(L, FontFlagsToString(flags));
    return 3;
}

// ref: FUN_1409d38b0
int GetFontObject(lua_State* L) {
    const char* usage = "Usage: local font = self:GetFontObject(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    CSimpleFont* font = html->GetFontObject(type);
    RaisePendingError(L, html, "GetFontObject");
    return PushObject(L, font);
}

// ref: FUN_1409d4670
int GetHyperlinkFormat(lua_State* L) {
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, "Usage: local format = self:GetHyperlinkFormat()");
    const char* format = html->m_hyperlinkFormat;
    RaisePendingError(L, html, "GetHyperlinkFormat");
    // FUN_1407cb130
    return PushStringOrNil(L, format);
}

// ref: FUN_1409d53f0
int GetIndentedWordWrap(lua_State* L) {
    const char* usage = "Usage: local wordWrap = self:GetIndentedWordWrap(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    bool wrap = html->GetIndentedWordWrap(type);
    RaisePendingError(L, html, "GetIndentedWordWrap");
    return PushBool(L, wrap);
}

// ref: FUN_1409d61a0
int GetJustifyH(lua_State* L) {
    const char* usage = "Usage: local justifyH = self:GetJustifyH(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t justify = html->GetJustifyH(type);
    RaisePendingError(L, html, "GetJustifyH");
    return PushJustify(L, justify);
}

// ref: FUN_1409d6f50
int GetJustifyV(lua_State* L) {
    const char* usage = "Usage: local justifyV = self:GetJustifyV(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t justify = html->GetJustifyV(type);
    RaisePendingError(L, html, "GetJustifyV");
    return PushJustify(L, justify);
}

// ref: FUN_1409d7d00
int GetShadowColor(lua_State* L) {
    const char* usage = "Usage: local r, g, b, a = self:GetShadowColor(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t color = *html->GetShadowColor(type);
    RaisePendingError(L, html, "GetShadowColor");
    // FUN_1408c6c60
    return PushColor(L, color);
}

// ref: FUN_1409d8ac0
int GetShadowOffset(lua_State* L) {
    const char* usage = "Usage: local x, y = self:GetShadowOffset(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    const float* offset = html->GetShadowOffset(type);
    RaisePendingError(L, html, "GetShadowOffset");
    // FUN_140800c50
    PushFloat(L, offset[0]);
    PushFloat(L, offset[1]);
    return 2;
}

// ref: FUN_1409d9880
int GetSpacing(lua_State* L) {
    const char* usage = "Usage: local spacing = self:GetSpacing(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    float spacing = html->GetSpacing(type);
    RaisePendingError(L, html, "GetSpacing");
    return PushUIUnits(L, spacing);
}

// ref: FUN_1409da640
int GetTextColor(lua_State* L) {
    const char* usage = "Usage: local r, g, b, a = self:GetTextColor(textType)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t color = *html->GetTextColor(type);
    RaisePendingError(L, html, "GetTextColor");
    return PushColor(L, color);
}

struct TEXTDATAARGS {
    CSimpleHTML* m_html;                                                    // 0x00
    blz::vector<CSimpleHTML::HTMLContentNode, g_htmlContentNodeTag> m_nodes; // 0x08
};

// ref: FUN_1408de130
int32_t GetTextDataBody(lua_State* L, void* context) {
    const char* usage = "Usage: local content = self:GetTextData()";
    auto args = static_cast<TEXTDATAARGS*>(context);
    int32_t index = 1;
    args->m_html = HtmlSelf(L, &index, usage);
    blz::vector<CSimpleHTML::HTMLContentNode, g_htmlContentNodeTag> nodes;
    args->m_html->GetTextData(&nodes);
    args->m_nodes.swap(nodes);
    RaisePendingError(L, args->m_html, "GetTextData");
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_createtable(L, static_cast<int32_t>(args->m_nodes.size()), 0);
    int32_t table = lua_gettop(L);
    int32_t count = 0;
    for (CSimpleHTML::HTMLContentNode& node : args->m_nodes) {
        lua_pushinteger(L, ++count);
        lua_createtable(L, 0, 3);
        int32_t entry = lua_gettop(L);
        PushStringOrNil(L, node.m_text);
        lua_setfield(L, entry, "text");
        // FUN_1408c6cc0
        PushStringOrNil(L, TextTypeToString(node.m_textType));
        lua_setfield(L, entry, "type");
        PushJustify(L, node.m_justify);
        lua_setfield(L, entry, "align");
        lua_settable(L, table);
    }
    return 1;
}

// ref: FUN_1409db400
int GetTextData(lua_State* L) {
    TEXTDATAARGS args = {};
    return ProtectedCall(L, GetTextDataBody, &args);
}

// ref: FUN_1409dc160
int SetFont(lua_State* L) {
    const char* usage = "Usage: self:SetFont(textType, fontFile, height, flags)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    const char* path = RequiredText(L, &index, usage);
    // FUN_1408c6720
    float height = 0.0f;
    if (lua_type(L, index) == LUA_TNUMBER) {
        height = CSimpleFont_ConvertHeight(static_cast<float>(lua_tonumber(L, index)));
    }
    index++;
    // FUN_1408c61d0
    uint8_t flags = 0;
    if (lua_type(L, index) == LUA_TSTRING) {
        StringToFontFlags(&flags, lua_tolstring(L, index, nullptr));
    }
    html->SetFont(type, path, height, &flags);
    RaisePendingError(L, html, "SetFont");
    return 0;
}

// ref: FUN_1409dcfe0
int SetFontObject(lua_State* L) {
    const char* usage = "Usage: self:SetFontObject(textType, font)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    CSimpleFont* font = nullptr;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if ((font = FontArgument(L, index)) == nullptr) {
        luaL_argerror(L, index, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        index++;
    }
    html->SetFontObject(type, font);
    RaisePendingError(L, html, "SetFontObject");
    return 0;
}

// ref: FUN_1409dde80
int SetHyperlinkFormat(lua_State* L) {
    const char* usage = "Usage: self:SetHyperlinkFormat(format)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    const char* format = RequiredText(L, &index, usage);
    html->SetHyperlinkFormat(format);
    RaisePendingError(L, html, "SetHyperlinkFormat");
    return 0;
}

// ref: FUN_1409dec70
int SetIndentedWordWrap(lua_State* L) {
    const char* usage = "Usage: self:SetIndentedWordWrap(textType, wordWrap)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    bool wrap = RequiredBool(L, &index, usage);
    html->SetIndentedWordWrap(type, wrap);
    RaisePendingError(L, html, "SetIndentedWordWrap");
    return 0;
}

// ref: FUN_1409dfa80
int SetJustifyH(lua_State* L) {
    const char* usage = "Usage: self:SetJustifyH(textType, justifyH)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    html->SetJustifyH(type, justify);
    RaisePendingError(L, html, "SetJustifyH");
    return 0;
}

// ref: FUN_1409e0860
int SetJustifyV(lua_State* L) {
    const char* usage = "Usage: self:SetJustifyV(textType, justifyV)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    html->SetJustifyV(type, justify);
    RaisePendingError(L, html, "SetJustifyV");
    return 0;
}

struct COLORARGS {
    CSimpleHTML* m_html;        // 0x00
    uint8_t m_textType;         // 0x08
    uint8_t m_pad09[3];
    uint32_t m_rgb;             // 0x0c
    uint8_t m_alpha[2];         // 0x10 value, present
};

template <void (CSimpleHTML::*Setter)(uint8_t, uint32_t, const uint8_t*)>
int32_t SetColorBody(lua_State* L, void* context, const char* usage, const char* method) {
    auto args = static_cast<COLORARGS*>(context);
    int32_t index = 1;
    args->m_html = HtmlSelf(L, &index, usage);
    args->m_textType = RequiredTextType(L, &index, usage);
    // FUN_140a50ab0
    args->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, args->m_alpha);
    (args->m_html->*Setter)(args->m_textType, args->m_rgb, args->m_alpha);
    RaisePendingError(L, args->m_html, method);
    return 0;
}

// ref: FUN_1408def40
int32_t SetShadowColorBody(lua_State* L, void* context) {
    return SetColorBody<&CSimpleHTML::SetShadowColor>(L, context, "Usage: self:SetShadowColor(textType, color [, a])", "SetShadowColor");
}

// ref: FUN_1409e1640
int SetShadowColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetShadowColorBody, &args);
}

// ref: FUN_1409e2370
int SetShadowOffset(lua_State* L) {
    const char* usage = "Usage: self:SetShadowOffset(textType, offset)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    float offset[2] = {};
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isnumber(L, index) || !lua_isnumber(L, index + 1)) {
        luaL_argerror(L, index, usage);
    } else if (!lua_checkstack(L, 2)) {
        luaL_error(L, "stack overflow");
    } else {
        offset[0] = static_cast<float>(lua_tonumber(L, index));
        offset[1] = static_cast<float>(lua_tonumber(L, index + 1));
        index += 2;
    }
    html->SetShadowOffset(type, offset);
    RaisePendingError(L, html, "SetShadowOffset");
    return 0;
}

// ref: FUN_1409e3270
int SetSpacing(lua_State* L) {
    const char* usage = "Usage: self:SetSpacing(textType, spacing)";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    uint8_t type = RequiredTextType(L, &index, usage);
    float spacing = 0.0f;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isnumber(L, index)) {
        luaL_argerror(L, index, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        spacing = ToLayoutUnits(lua_tonumber(L, index));
        index++;
    }
    html->SetSpacing(type, spacing);
    RaisePendingError(L, html, "SetSpacing");
    return 0;
}

// ref: FUN_1409e40f0
int SetText(lua_State* L) {
    const char* usage = "Usage: self:SetText(text [, ignoreMarkup])";
    int32_t index = 1;
    CSimpleHTML* html = HtmlSelf(L, &index, usage);
    const char* text = RequiredText(L, &index, usage);
    bool ignoreMarkup = false;
    if (lua_type(L, index) >= 1) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            ignoreMarkup = lua_toboolean(L, index) != 0;
        }
    }
    html->SetTextFromScript(text, ignoreMarkup);
    RaisePendingError(L, html, "SetText");
    return 0;
}

// ref: FUN_1408dfde0
int32_t SetTextColorBody(lua_State* L, void* context) {
    return SetColorBody<&CSimpleHTML::SetTextColor>(L, context, "Usage: self:SetTextColor(textType, color [, a])", "SetTextColor");
}

// ref: FUN_1409e4f30
int SetTextColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetTextColorBody, &args);
}

// DAT_142cf8c30
const luaL_Reg s_htmlMethods[] = {
    { "GetContentHeight", GetContentHeight },
    { "GetFont", GetFont },
    { "GetFontObject", GetFontObject },
    { "GetHyperlinkFormat", GetHyperlinkFormat },
    { "GetIndentedWordWrap", GetIndentedWordWrap },
    { "GetJustifyH", GetJustifyH },
    { "GetJustifyV", GetJustifyV },
    { "GetShadowColor", GetShadowColor },
    { "GetShadowOffset", GetShadowOffset },
    { "GetSpacing", GetSpacing },
    { "GetTextColor", GetTextColor },
    { "GetTextData", GetTextData },
    { "SetFont", SetFont },
    { "SetFontObject", SetFontObject },
    { "SetHyperlinkFormat", SetHyperlinkFormat },
    { "SetIndentedWordWrap", SetIndentedWordWrap },
    { "SetJustifyH", SetJustifyH },
    { "SetJustifyV", SetJustifyV },
    { "SetShadowColor", SetShadowColor },
    { "SetShadowOffset", SetShadowOffset },
    { "SetSpacing", SetSpacing },
    { "SetText", SetText },
    { "SetTextColor", SetTextColor },
};
static_assert(sizeof(s_htmlMethods) / sizeof(s_htmlMethods[0]) == 0x17, "the SimpleHTML table has 0x17 methods");

}

// ref: FUN_140a51020
void CSimpleHTML_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_htmlMethods, 0x17);
}
