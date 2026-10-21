#include "ui/CSimpleFontString.hpp"

#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "base/DataAllocator.hpp"
#include "base/Handle.hpp"
#include "base/Status.hpp"
#include "blz/String.hpp"
#include "blz/Vector.hpp"
#include "client/GxWindow.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/TextureAtlas.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

int64_t CSimpleFontString::s_count;
int32_t CSimpleFontString::s_metaTable = -2;
int32_t CSimpleFontString::s_restrictedMetaTable = -2;
uint8_t CSimpleFontString::s_drawOffscreen;


// Expands the localization grammar codes (|1 .. |7) of a text into the processor's buffer
// (FUN_141652250 on the processor at 0x142cce910).
const char* FontStringProcessGrammar(const char* text);

namespace {

const char s_sourceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleFontString.cpp";
const char s_embeddedTag[] = ".?AV?$TSList@VCSimpleEmbeddedTexture@@V?$TSGetExplicitLink@VCSimpleEmbeddedTexture@@@@$07@@";
const char s_boundsTag[] = ".?AUuiBoundsRect@@";

// The pool (0x142e97be0).
CDataAllocator s_fontStringHeap(0x2e8, 0x160);  // DAT_142e97be0

// The text cut to fit, with its "..." (0x142e97c20).
char s_fittedText[0x2000];

// The display text with its |K codes expanded (0x142cf3310).
blz::inline_string<0x2000> s_expandedText;

// Embedded texture nodes nobody uses (0x142cce8d8, link offset 8).
struct EMBEDDEDCACHE {
    TSExplicitList<CSimpleEmbeddedTexture> m_list;
    EMBEDDEDCACHE() {
        this->m_list.InitEmpty(8);
    }
} s_embeddedCache;

// Font strings waiting for a font family member's file (0x142cce8f0, link offset 0x260).
struct PENDINGSTRINGS {
    TSExplicitList<CSimpleFontString> m_list;
    PENDINGSTRINGS() {
        this->m_list.InitEmpty(0x260);
    }
} s_pendingStrings;

// The family member slot for each script a font lacks (0x1426cb2c0): Korean, the Chinese
// alphabets, Russian.
const int32_t s_scriptMember[] = { 1, 2, 4, 0 };

constexpr float EPSILON = 2.3841858e-07f;      // 0x14261a954
constexpr float SIZE_EPSILON = 1.0e-05f;       // 0x1426c75c4

typedef const char* (*GXUSTRINGRENDERER)(int32_t kind, int32_t id);

float Abs(float value) {
    return value < 0.0f ? -value : value;
}

}

// A height in UI units rounded up to whole pixels of the screen (FUN_1405f3460). The
// reference reads the first half of the resolution here.
// ref: FUN_1405f3460
float SnapToPixels(float height) {
    if (Abs(height) < EPSILON) {
        return height;
    }
    GXSIZE size;
    WindowGetResolution(&size);
    float pixels = static_cast<float>(size.m_width);
    if (Abs(pixels) < EPSILON) {
        return height;
    }
    float diagonal = g_coordinateSpace.m_diagonalHeight;
    float scaled = (height / diagonal) * pixels;
    if (0.0f < scaled) {
        return (static_cast<float>(static_cast<int32_t>(static_cast<int64_t>(scaled + 0.99995f))) / pixels) * diagonal;
    }
    return (static_cast<float>(-static_cast<int32_t>(static_cast<int64_t>(-scaled))) / pixels) * diagonal;
}

namespace {

// Copies at most `size` - 1 characters and terminates.
void CopyBounded(char* dest, const char* source, uint32_t size) {
    if (size == 0) {
        return;
    }
    uint32_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

// The region's colour, or the gradient's first corner.
uint32_t RegionColor(CSimpleRegion* region) {
    if ((region->m_regionFlags & CSimpleRegion::REGIONFLAG_GRADIENT) != 0) {
        return region->m_gradient != nullptr ? region->m_gradient->m_colors[0] : 0xffffffff;
    }
    return region->m_color;
}

// The shadow colour with the frame's alpha multiplied into its own.
uint32_t ShadowColorWithAlpha(CSimpleFontString* string) {
    uint32_t color = string->m_shadowColor;
    uint8_t alpha = string->m_parent->GetEffectiveAlpha();
    uint32_t shadowAlpha = (static_cast<uint32_t>(color >> 24) * alpha) / 0xff;
    return (color & 0xffffff) | (shadowAlpha << 24);
}

// The name messages use: the object's, or its type's (FUN_140628560).
const char* DisplayName(CSimpleFontString* string) {
    const char* name = string->GetName();
    return name != nullptr ? name : string->GetObjectTypeName();
}

// Reads the number of a |K code: the kind letter is at `kind`, the digits follow. Returns the
// last character read (the last digit, or the kind letter when there are none).
const char* ParseKCode(const char* kind, int32_t* id) {
    int32_t value = 0;
    uint8_t c = '0';
    const char* last = kind;
    while (true) {
        value = c + (value * 5 - 0x18) * 2;
        c = static_cast<uint8_t>(last[1]);
        if (c < '0' || '9' < c) {
            break;
        }
        last++;
    }
    *id = value;
    return last;
}

// Copies the display text with each |K..|k code replaced by what the string renderer makes of
// it; true when there was one (FUN_141cb2d80).
// ref: FUN_141cb2d80
bool FontStringExpandCodes(const char* text, blz::string_header* out) {
    out->m_data[0] = '\0';
    out->m_size = 0;
    if (text == nullptr || *text == '\0') {
        return false;
    }
    auto renderer = reinterpret_cast<GXUSTRINGRENDERER>(GxuFontGetStringRenderer());
    bool expanded = false;
    while (*text != '\0') {
        char c = *text;
        if (c == '|' && text[1] != '\0') {
            if (text[1] != 'K') {
                blz::string_append(out, text, 2);
                text += 2;
                continue;
            }
            int32_t id = 0;
            const char* last = ParseKCode(text + 2, &id);
            if (last[1] != '|' || last[2] != 'k') {
                blz::string_append(out, text, 2);
                text += 2;
                continue;
            }
            expanded = true;
            const char* name = renderer(static_cast<uint8_t>(text[2]), id);
            text = last + 3;
            blz::string_append(out, name, std::strlen(name));
            continue;
        }
        blz::string_append(out, text, 1);
        text++;
    }
    return expanded;
}

// The `index`th hyperlink of a text, looking into what |K codes expand to: its link and its text
// with the colour code around it (FUN_140625340).
// ref: FUN_140625340
bool FontStringFindHyperlink(const char* text, uint32_t flags, int32_t index, int32_t* counter, GXUSTRINGRENDERER renderer,
    blz::string* link, blz::string* linkText) {
    bool inLink = false;
    const char* colorStart = nullptr;
    if (text == nullptr) {
        return false;
    }
    while (*text != '\0') {
        int32_t advance = 0;
        uint32_t color = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(text, &advance, &color, flags, &code);
        const char* next = text + advance;
        const char* keep = colorStart;
        if (kind == 1) {
            keep = text;
        } else if (kind == 2) {
            keep = nullptr;
        } else if (kind == 5) {
            if (!inLink) {
                inLink = true;
                int32_t found = (*counter)++;
                if (found == index) {
                    // |H<link>|h
                    blz::string_assign(reinterpret_cast<blz::string_header*>(link), text + 2,
                        static_cast<uint64_t>(advance - 2 - 2));
                    const char* end = std::strstr(next, "|h");
                    if (end == nullptr) {
                        linkText->m_data[0] = '\0';
                        linkText->m_size = 0;
                    } else {
                        const char* stop = end + 2;
                        const char* start = text;
                        if (std::strncmp(stop, "|r", 2) == 0 && colorStart != nullptr) {
                            stop += 2;
                            start = colorStart;
                        }
                        blz::string_assign(reinterpret_cast<blz::string_header*>(linkText), start,
                            static_cast<uint64_t>(stop - start));
                    }
                    return true;
                }
            }
        } else if (kind == 6) {
            inLink = false;
        } else if (kind == 9 && !inLink) {
            int32_t id = 0;
            const char* last = ParseKCode(text + 2, &id);
            if (last[1] == '|' && last[2] == 'k') {
                const char* name = renderer(static_cast<uint8_t>(text[2]), id);
                blz::string expanded;
                expanded.construct(name, std::strlen(name));
                int32_t inner = 0;
                bool found = FontStringFindHyperlink(expanded.m_data, flags, index - *counter, &inner, renderer, link, linkText);
                index += inner;
                if (found) {
                    return true;
                }
            }
        }
        colorStart = keep;
        text = next;
    }
    return false;
}

// Sets an embedded texture up from its code: the file or atlas, the anchor its justification
// gives, the size, coordinates and colour, and the string's layer (FUN_140626660).
// ref: FUN_140626660
void FontStringSetUpEmbeddedTexture(CSimpleEmbeddedTexture* node, CSimpleFontString* string, const GXUEMBEDDEDTEXTURE* info) {
    CSimpleTexture* texture = node != nullptr ? node->m_texture : nullptr;
    if (string == nullptr || info == nullptr || texture == nullptr) {
        if (texture != nullptr) {
            texture->Hide();
        }
        return;
    }
    bool ok;
    char name[0x104];
    if (info->m_path == nullptr) {
        if (info->m_atlasId == 0) {
            if (info->m_atlas == nullptr) {
                ok = texture->SetTextureById(info->m_fileDataId, 0, 0, 1, true, false);
            } else {
                uint32_t size = static_cast<uint32_t>(info->m_atlasLength) + 1;
                CopyBounded(name, info->m_atlas, size < 0x104 ? size : 0x104);
                TEXTUREATLASSCALE scale = { CSimpleTop::s_instance->GetRegionLayoutVersion(texture), 1 };
                int32_t member = 0;
                ok = texture->SetAtlas(TextureAtlasFindByName(&member, name, &scale), 1, false, 0, 0);
            }
        } else {
            TEXTUREATLASSCALE scale = { 0, 0 };
            int32_t member = 0;
            ok = texture->SetAtlas(TextureAtlasFindByMember(&member, info->m_atlasId, &scale), 1, false, 0, 0);
        }
    } else {
        uint32_t size = static_cast<uint32_t>(info->m_pathLength) + 1;
        CopyBounded(name, info->m_path, size < 0x104 ? size : 0x104);
        ok = texture->SetTextureByName(name, 0, 0, 1, true, false);
    }
    if (!ok) {
        texture->Hide();
        return;
    }

    const COORDINATESPACE& space = g_coordinateSpace;
    float bottom = info->m_rect[0] * space.m_diagonalHeight;
    float left = space.m_diagonalWidth * info->m_rect[1];
    float top = info->m_rect[2] * space.m_diagonalHeight;
    float right = space.m_diagonalWidth * info->m_rect[3];
    float offsetX = space.m_diagonalWidth * info->m_offsetX;
    float offsetY = space.m_diagonalHeight * info->m_offsetY;
    float width = space.m_diagonalWidth * info->m_width;
    float height = space.m_diagonalHeight * info->m_height;

    int32_t point = 0;
    float x = 0.0f;
    float y = 0.0f;
    switch (string->m_stringFlags & CSimpleFontString::FONTSTRING_JUSTIFY_H) {
    case TEXTBLOCK_JUSTIFY_LEFT:
        x = left + offsetX;
        break;
    case TEXTBLOCK_JUSTIFY_CENTER:
        point = 1;
        x = (right + left) * 0.5f + offsetX;
        break;
    case TEXTBLOCK_JUSTIFY_RIGHT:
        point = 2;
        x = right + offsetX;
        break;
    default:
        break;
    }
    switch (string->m_stringFlags & CSimpleFontString::FONTSTRING_JUSTIFY_V) {
    case CSimpleFontString::FONTSTRING_JUSTIFY_TOP:
        y = bottom + offsetY;
        break;
    case CSimpleFontString::FONTSTRING_JUSTIFY_MIDDLE: {
        float scale = string->m_layoutScale;
        point += 3;
        y = (string->GetStringHeight() * scale + height) * 0.5f + top + offsetY;
        break;
    }
    case CSimpleFontString::FONTSTRING_JUSTIFY_BOTTOM:
        point += 6;
        y = string->GetStringHeight() + top + offsetY;
        break;
    default:
        break;
    }

    texture->ClearAllPointsVirtual();
    texture->SetPoint(point, string, point, x, y, true);
    texture->SetWidth(width);
    texture->SetHeight(height);

    static const float s_fullTexCoords[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    texture->SetTexCoordsRect(info->m_hasTexCoords != 0 ? info->m_texCoords : s_fullTexCoords);

    uint32_t color = 0xffffffff;
    if (info->m_hasVertexColor != 0) {
        std::memcpy(&color, info->m_vertexColor, sizeof(color));
    }
    double alpha = 0.0;
    if (string->m_parent != nullptr) {
        double own = static_cast<double>(string->GetAlpha());
        alpha = static_cast<double>(string->m_parent->GetAlpha()) * (own * (1.0 / 65025.0));
    }
    uint32_t colorAlpha = static_cast<uint32_t>(static_cast<int32_t>(static_cast<double>(color >> 24) * alpha)) & 0xff;
    color = (color & 0xffffff) | (colorAlpha << 24);
    texture->CSimpleRegion::SetVertexColor(&color);
    texture->SetFrame(texture->m_parent, string->m_drawLayer, 7, (string->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0);
    texture->Show();
}

}

// ---- the grammar processor -------------------------------------------------------------------

namespace {

// The processor's buffer (0x142cce918, 30000 bytes after the vtable).
char s_grammarBuffer[30000 + 8];

}

// ref: FUN_141652250
const char* FontStringProcessGrammar(const char* text) {
    if (30000 < strlen(text)) {
        std::memcpy(s_grammarBuffer, text, 30000);
        s_grammarBuffer[30000 - 3] = '\0';
        s_grammarBuffer[30000 - 2] = '\0';
        s_grammarBuffer[30000 - 1] = '\0';
        text = s_grammarBuffer;
    }
    for (const char* c = text; *c != '\0'; c++) {
        if (*c != '|') {
            continue;
        }
        char code = c[1];
        if (code == '|') {
            c++;
            continue;
        }
        if (code >= '1' && code <= '7') {
            // The grammar codes (|1 a/an, |2 de/d', |3 declension, |4 plural, |5 case, |6 lower
            // case, |7 gender) each rewrite the text (FUN_141652490 .. FUN_1416530f0).
            FROST_UNIMPLEMENTED();
            return text;
        }
    }
    return text;
}

// ---- lifetime ----------------------------------------------------------------------------------

// ref: FUN_1406228b0
CSimpleFontString::CSimpleFontString(CSimpleFrame* frame, uint8_t drawLayer, bool show)
    : CSimpleRegion(frame, drawLayer, 0, show) {
    this->m_embeddedTextures.InitEmpty(8);
    s_count++;
}

CSimpleFontString::~CSimpleFontString() {
    this->ReleaseEmbeddedTextures();
    SMemFree(this->m_text, s_sourceFile, 0x13d, 0);
    this->m_text = nullptr;
    bool changed = this->m_font || CSimpleTop::s_instance->m_resizing;
    this->m_fontHeight = 0.0f;
    if (this->m_textBlock != nullptr && changed) {
        HandleClose(this->m_textBlock);
        this->m_textBlock = nullptr;
    }
    this->m_font.reset();
    if (this->m_fontRequest != nullptr) {
        operator delete(this->m_fontRequest);
    }
    s_count--;
    this->m_pendingLink.Unlink();
    // the embedded texture list (FUN_1402f9a70)
    this->m_embeddedTextures.UnlinkAll();
    this->m_embeddedTextures.m_terminator.Unlink();
}

void* CSimpleFontString::operator new(size_t size) {
    (void)size;
    return s_fontStringHeap.GetData(false, ".?AVCSimpleFontString@@", -2, 0x1f);
}

void CSimpleFontString::operator delete(void* string) {
    if (string != nullptr) {
        s_fontStringHeap.PutData(string);
    }
}

// ref: FUN_140623c70
void CSimpleFontString::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFontString_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFontString_FillScriptMethods, true);
}

// ref: FUN_140623cc0
void CSimpleFontString::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// The reference's type id and its magic-static guard (CSimpleFontString), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleFontStringType;  // DAT_142e97c08
int32_t g_cSimpleFontStringTypeGuard;  // DAT_142e97c0c

int32_t CSimpleFontString::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleFontStringType, &g_cSimpleFontStringTypeGuard);
}

// ref: FUN_140624c80
void CSimpleFontString::DestroyEmbeddedTextureCache() {
    while (CSimpleEmbeddedTexture* node = s_embeddedCache.m_list.Head()) {
        if (node->m_texture != nullptr) {
            delete node->m_texture;
        }
        node->m_link.Unlink();
        SMemFree(node, s_embeddedTag, -2, 0);
    }
}

// ref: FUN_140624a60
void CSimpleFontString::OnFontFileLoaded(void* path, void* buffer, uint32_t size) {
    auto face = static_cast<CHandleObject*>(GxuFontCreateFaceFromBuffer(static_cast<const char*>(path), buffer, size));
    while (CSimpleFontString* string = s_pendingStrings.m_list.Head()) {
        string->m_pendingLink.Unlink();
        if (string->m_text != nullptr && string->m_text[0] != '\0') {
            string->UpdateFamilyMember(string->GetDisplayText(), false, false);
        }
    }
    HandleClose(face);
}

// ref: FUN_140623360
void* CSimpleFontString::AllocFontFileBuffer(uint32_t size) {
    return SMemAlloc(size, 0x1f, s_sourceFile, 0, 0x534, 0);
}

void CSimpleFontString::OnFontFileError(void* path, void* buffer, uint32_t size, const char* error) {
    (void)path;
    (void)size;
    (void)error;
    if (buffer != nullptr) {
        SMemFree(buffer, s_sourceFile, 0x540, 0);
    }
}

void CSimpleFontString::OnFontFileCanceled(void* path, void* buffer, uint32_t size) {
    (void)path;
    (void)size;
    if (buffer != nullptr) {
        SMemFree(buffer, s_sourceFile, 0x54d, 0);
    }
}

// ---- FrameScript_Object, CScriptObject and CScriptRegion -----------------------------------------

// ref: FUN_140625d30
int32_t CSimpleFontString::GetScriptMetaTable() {
    if ((this->FrameScript_Object::m_flags & 1) == 0 && (this->FrameScript_Object::m_flags & 4) == 0) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_140626be0
bool CSimpleFontString::IsObjectTypeName(const char* name) {
    return _stricmp(name, "FontString") == 0 || _stricmp(name, "Region") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140626ae0
bool CSimpleFontString::IsObjectType(int32_t type) {
    return type == CSimpleFontString::GetObjectType() || type == CScriptRegion::GetObjectType()
        || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_140625c20
const char* CSimpleFontString::GetObjectTypeName() {
    return "FontString";
}

// ref: FUN_140625910
uint32_t CSimpleFontString::GetObjectSize() {
    return 0x2e8;
}

// The embedded textures join a list of textures (blz::vector<CTexture*>).
// ref: FUN_140623e90
void CSimpleFontString::Unknown98(void* argument) {
    auto list = static_cast<blz::vector<CTexture*, s_textureListTag>*>(argument);
    for (CSimpleEmbeddedTexture* node = this->m_embeddedTextures.Head(); node != nullptr;
         node = this->m_embeddedTextures.Link(node)->Next()) {
        if (node->m_texture != nullptr && node->m_texture->m_texture != nullptr) {
            list->push_back(node->m_texture->m_texture);
        }
    }
}

// The template it inherits (a named font object, or an XML node), the region's settings, then
// the font string's: text scale, rotation, text, wrapping, bytes, lines, the font and its
// attributes, alpha, colour and shadow.
// ref: FUN_140626d70
void CSimpleFontString::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    auto xml = static_cast<XMLFONTSTRING*>(node);
    if ((node->m_attributes & 0x100) != 0) {
        CSimpleFont* font = CSimpleFont::GetFont(node->m_inherits.m_text, false);
        if (font != nullptr) {
            if ((this->m_textFlags & TEXT_FONTABLE) != 0) {
                this->SetFontObject(font);
            }
        } else {
            char inherits[0x80];
            CopyBounded(inherits, node->m_inherits.m_text, sizeof(inherits));
            const char* taint = nullptr;
            bool busy = true;
            XMLOBJECT* parent = FrameXML_AcquireVirtual(inherits, &taint, &busy);
            if (parent == nullptr) {
                status->Add(1, "Couldn't find inherited node: %s", inherits);
            } else if (busy) {
                status->Add(1, "Recursively inherited node: %s", inherits);
            } else {
                this->LoadXML(parent, status, nullptr);
            }
            if (parent != nullptr && !busy) {
                FrameXML_ReleaseVirtual(inherits);
            }
        }
    }

    this->CSimpleRegion::LoadXML(node, status, intrinsic);
    if ((node->m_flags & 0x800000) == 0) {
        return;
    }

    if ((xml->m_regionFlags & 2) != 0) {
        if ((xml->m_regionFlags & 0x10) == 0) {
            this->Show();
        } else {
            this->Hide();
        }
    }

    this->m_textScale = (xml->m_fontStringFlags & 0x40) != 0 ? xml->m_textScale : 1.0f;
    this->m_rotation = (xml->m_fontStringFlags & 0x20) != 0 ? xml->m_rotation : 0.0f;

    if ((xml->m_fontStringFlags & 4) != 0) {
        const char* text = FrameScript_GetText(xml->m_text.m_text, -1, 0);
        if (text == nullptr || *text == '\0') {
            text = xml->m_text.m_text;
        }
        this->SetText(text, true);
    }

    uint32_t fontFlags = xml->m_fontFlags;
    if ((fontFlags & 4) != 0) {
        this->m_inheritedFlags &= ~FONTSTRING_NON_SPACE_WRAP;
        this->SetStringFlag(FONTSTRING_NON_SPACE_WRAP, (fontFlags & 0x40) != 0);
    }

    uint32_t stringFlags = xml->m_fontStringFlags;
    if ((stringFlags & 8) != 0) {
        this->m_inheritedFlags &= ~FONTSTRING_NO_WORD_WRAP;
        this->SetStringFlag(FONTSTRING_NO_WORD_WRAP, (stringFlags & 0x10) == 0);
    }

    if ((xml->m_fontStringFlags & 1) != 0) {
        this->SetMaxBytes(static_cast<uint32_t>(xml->m_bytes));
    }

    if ((xml->m_fontStringFlags & 2) != 0) {
        this->m_maxLines = (this->m_maxLines & 0xff000000) | (static_cast<uint32_t>(xml->m_maxLines) & 0xffffff);
    }

    if ((this->m_textFlags & TEXT_FONTABLE) == 0) {
        return;
    }

    if ((xml->m_fontFlags & 0x80) != 0) {
        const char* path = xml->m_font.m_text;
        CSimpleFont* font = CSimpleFont::GetFont(path, false);
        if (font != nullptr) {
            this->SetFontObject(font);
        } else if (*path != '\0') {
            float height = 0.0f;
            XMLOBJECT* child = XMLObjectFindChild(node, 100);
            if (child != nullptr) {
                XMLValueGet(static_cast<XMLVALUE*>(child), &height, status);
                if (120.0f <= height) {
                    height = 120.0f;
                }
            }
            if (height <= 0.0f) {
                status->Add(1, "Font %s: Missing font height in %s element", DisplayName(this), node->m_tag.m_text);
                return;
            }
            uint8_t flags = 0;
            if ((xml->m_fontFlags & 0x100) != 0) {
                const char* outline = xml->m_outline.m_text;
                if (_stricmp(outline, "NORMAL") == 0) {
                    flags = 1;
                } else if (_stricmp(outline, "THICK") == 0) {
                    flags = 5;
                }
            }
            uint32_t attributes = xml->m_fontFlags;
            if ((attributes & 0x600) == 0x600) {
                flags |= 2;
            }
            if ((attributes & 0x3000) == 0x3000) {
                flags |= 8;
            }
            if ((attributes & 0xc000) == 0xc000) {
                flags |= 0x10;
            }
            if (!this->SetFont(path, height, flags, 0)) {
                status->Add(1, "Font %s: Unable to load font file %s", DisplayName(this), path);
            } else {
                this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FONT;
            }
        }
    }

    if ((xml->m_fontFlags & 0x20) != 0) {
        const COORDINATESPACE& space = g_coordinateSpace;
        this->SetSpacing((xml->m_spacing / (space.m_aspectCompensation * 1024.0f)) * space.m_diagonalWidth);
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SPACING;
    }

    uint32_t justify = 0;
    if ((xml->m_fontFlags & 1) != 0 && StringToJustify(xml->m_justifyV.m_text, &justify)) {
        this->m_inheritedFlags &= ~FONTSTRING_JUSTIFY_V;
        this->SetJustifyV(justify & FONTSTRING_JUSTIFY_V);
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    }
    if ((xml->m_fontFlags & 2) != 0 && StringToJustify(xml->m_justifyH.m_text, &justify)) {
        this->m_inheritedFlags &= ~FONTSTRING_JUSTIFY_H;
        this->SetJustifyH(justify & FONTSTRING_JUSTIFY_H);
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    }

    if ((xml->m_fontFlags & 8) != 0) {
        this->m_inheritedFlags &= ~FONTSTRING_INDENTED_WORD_WRAP;
        this->SetStringFlag(FONTSTRING_INDENTED_WORD_WRAP, (xml->m_fontFlags & 0x10) != 0);
    }

    if ((xml->m_regionFlags & 1) != 0) {
        float alpha = xml->m_alpha;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        } else if (1.0f <= alpha) {
            alpha = 1.0f;
        }
        this->SetAlpha(static_cast<uint8_t>(static_cast<int32_t>(alpha * 255.0f)));
    }

    if ((xml->m_regionFlags & 0x20) != 0) {
        uint8_t ignore = static_cast<uint8_t>(xml->m_regionFlags >> 6);
        if (((this->m_regionFlags >> 1 ^ ignore) & 1) != 0) {
            this->m_regionFlags = static_cast<uint8_t>(((ignore * 2) ^ this->m_regionFlags) & 2 ^ this->m_regionFlags);
            this->UpdateEffectiveAlpha();
        }
    }

    if (static_cast<int8_t>(xml->m_regionFlags) < 0) {
        this->SetIgnoreParentScale(((xml->m_regionFlags >> 8) & 1) != 0);
    }

    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_COLOR) {
            uint32_t color = XmlColorPack(static_cast<XMLCOLOR*>(child));
            this->CSimpleRegion::SetVertexColor(&color);
            this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
        } else if (child->m_type == XMLTYPE_SHADOW) {
            uint32_t color = 0xff000000;
            float offset[2] = { 1.0f, -1.0f };
            XMLOBJECT* shadowColor = XMLObjectFindChild(child, XMLTYPE_COLOR);
            if (shadowColor != nullptr) {
                color = XmlColorPack(static_cast<XMLCOLOR*>(shadowColor));
            }
            if (!XMLOffsetGet(static_cast<XMLDIMENSION*>(child), &offset[0], &offset[1], status)) {
                XMLOBJECT* dimension = XMLObjectFindChild(child, 0x97);
                if (dimension != nullptr && dimension->m_child != nullptr) {
                    auto value = static_cast<XMLDIMENSION*>(dimension->m_child);
                    offset[0] = value->m_x;
                    offset[1] = value->m_y;
                }
            }
            this->SetShadow(&color, offset);
            this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
        }
    }
}

// Anchored to its frame by its justification when nothing anchored it.
// ref: FUN_140628040
void CSimpleFontString::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    if (this->m_parent != nullptr && !this->IsAnchored()) {
        CLayoutFrame* relative = this->m_parent;
        int32_t point;
        if ((this->m_stringFlags & TEXTBLOCK_JUSTIFY_LEFT) != 0) {
            point = 3;
        } else if ((this->m_stringFlags & TEXTBLOCK_JUSTIFY_RIGHT) != 0) {
            point = 5;
        } else {
            point = 4;
        }
        this->SetPoint(point, relative, point, 0.0f, 0.0f, true);
    }
    this->CScriptRegion::PostLoadXML(node, status);
}

// A press on a hyperlink is remembered for the release.
// ref: FUN_140627cc0
bool CSimpleFontString::OnMouseDown(MOUSEEVENT* event, const char* button) {
    uint32_t link = (this->m_textFlags >> 4) & 0xffff;
    if (link == 0xffff) {
        this->CScriptRegion::OnMouseDown(event, button);
        return false;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_unknown318 == nullptr) {
        top->m_unknown318 = this;
        top->m_unknown320 = link;
        if (top->m_unknown324 == 0) {
            top->m_unknown324 = 1;
        }
    }
    return false;
}

// A release over the pressed hyperlink runs the frame's OnHyperlinkClick.
// ref: FUN_140627d10
bool CSimpleFontString::OnMouseUp(MOUSEEVENT* event, const char* button) {
    float point[2] = { event->m_x, event->m_y };
    uint32_t index = 0;
    CRect rect = {};
    bool found = this->GetHyperlinkAt(point, &index, &rect);
    CSimpleTop* top = CSimpleTop::s_instance;
    if (found) {
        if (top->m_unknown318 != this) {
            this->CScriptRegion::OnMouseUp(event, button);
            return false;
        }
        bool same = top->m_unknown324 == 0 ? !found : index == top->m_unknown320;
        if (same) {
            top->m_unknown318 = nullptr;
            if (top->m_unknown324 != 0) {
                top->m_unknown324 = 0;
            }
            GXUHYPERLINKINFO* links = nullptr;
            GxuFontStringGetHyperlinks(this->m_textBlock != nullptr ? this->m_textBlock->m_string : nullptr, &links);
            GXUHYPERLINKINFO* info = &links[index];
            if (button == nullptr) {
                button = MouseButtonToString(event->m_button);
            }
            if ((this->m_textFlags & TEXT_EXPANDED) == 0) {
                FRAMESCRIPT_LSTRING link = { info->m_link, static_cast<uint32_t>(info->m_linkLength) };
                FRAMESCRIPT_LSTRING text = { info->m_text, static_cast<uint32_t>(info->m_textLength) };
                this->m_parent->RunOnHyperlinkClick(&link, &text, button, this, &rect);
            } else {
                blz::string linkString;
                blz::string textString;
                const char* source = this->m_text != nullptr && this->m_text[0] != '\0' ? this->m_text : nullptr;
                int32_t counter = 0;
                auto renderer = reinterpret_cast<GXUSTRINGRENDERER>(GxuFontGetStringRenderer());
                if (FontStringFindHyperlink(source, TextBlockConvertFlags(this->m_stringFlags), static_cast<int32_t>(index),
                        &counter, renderer, &linkString, &textString)) {
                    FRAMESCRIPT_LSTRING link = { linkString.m_data, linkString.m_size };
                    FRAMESCRIPT_LSTRING text = { textString.m_data, textString.m_size };
                    this->m_parent->RunOnHyperlinkClick(&link, &text, button, this, &rect);
                }
            }
            return false;
        }
    }
    if (top->m_unknown318 == this) {
        top->m_unknown318 = nullptr;
        if (top->m_unknown324 != 0) {
            top->m_unknown324 = 0;
        }
    }
    this->CScriptRegion::OnMouseUp(event, button);
    return false;
}

// Shown font strings answer over their hyperlinks: 1 the one under the mouse, 2 another.
// ref: FUN_140629a70
int32_t CSimpleFontString::HitTest(int32_t type, MOUSEEVENT* event, bool force) {
    if ((this->m_layoutState & LAYOUT_SHOWN) != 0) {
        float point[2] = { event->m_x, event->m_y };
        uint32_t index = 0;
        if (this->GetHyperlinkAt(point, &index, nullptr)) {
            uint32_t current = (this->m_textFlags >> 4) & 0xffff;
            if (current != 0xffff && index != current) {
                return 2;
            }
            return 1;
        }
    }
    return this->CScriptRegion::HitTest(type, event, force);
}

// The hyperlink under the mouse runs the frame's OnHyperlinkEnter.
// ref: FUN_1406279e0
void CSimpleFontString::OnEnter(bool motion) {
    MOUSEEVENT& mouse = CSimpleTop::s_instance->m_mouseEvent;
    float point[2] = { mouse.m_x, mouse.m_y };
    uint32_t index = 0;
    CRect rect = {};
    if (this->GetHyperlinkAt(point, &index, &rect)) {
        GXUHYPERLINKINFO* links = nullptr;
        GxuFontStringGetHyperlinks(this->m_textBlock != nullptr ? this->m_textBlock->m_string : nullptr, &links);
        GXUHYPERLINKINFO* info = &links[index];
        if ((this->m_textFlags & TEXT_EXPANDED) == 0) {
            this->m_textFlags = ((index & 0xffff) << 4) | (this->m_textFlags & 0xfff0000f);
            FRAMESCRIPT_LSTRING link = { info->m_link, static_cast<uint32_t>(info->m_linkLength) };
            FRAMESCRIPT_LSTRING text = { info->m_text, static_cast<uint32_t>(info->m_textLength) };
            this->m_parent->RunOnHyperlinkEnter(&link, &text, this, &rect);
        } else {
            blz::string linkString;
            blz::string textString;
            const char* source = this->m_text != nullptr && this->m_text[0] != '\0' ? this->m_text : nullptr;
            int32_t counter = 0;
            auto renderer = reinterpret_cast<GXUSTRINGRENDERER>(GxuFontGetStringRenderer());
            if (FontStringFindHyperlink(source, TextBlockConvertFlags(this->m_stringFlags), static_cast<int32_t>(index),
                    &counter, renderer, &linkString, &textString)) {
                this->m_textFlags = ((index << 4) ^ this->m_textFlags) & 0xffff0 ^ this->m_textFlags;
                FRAMESCRIPT_LSTRING link = { linkString.m_data, linkString.m_size };
                FRAMESCRIPT_LSTRING text = { textString.m_data, textString.m_size };
                this->m_parent->RunOnHyperlinkEnter(&link, &text, this, &rect);
            }
        }
    }
    this->CScriptRegion::OnEnter(motion);
}

// ref: FUN_140627c50
void CSimpleFontString::OnLeave(bool motion) {
    uint32_t flags = this->m_textFlags;
    if (this->m_parent == nullptr) {
        this->m_textFlags = flags | TEXT_HYPERLINK_MASK;
    } else if ((flags & TEXT_HYPERLINK_MASK) != TEXT_HYPERLINK_MASK) {
        this->m_textFlags = flags | TEXT_HYPERLINK_MASK;
        this->m_parent->RunOnHyperlinkLeave();
    }
    this->CScriptRegion::OnLeave(motion);
}

// ---- CSimpleRegion -------------------------------------------------------------------------------

// ref: FUN_140629010
void CSimpleFontString::SetVertexColor(uint32_t rgb, const uint8_t* alpha) {
    this->CSimpleRegion::SetVertexColor(rgb, alpha);
    this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
}

// The colour and shadow go to the text block, and to the embedded textures' alpha.
// ref: FUN_140627600
void CSimpleFontString::UpdateEffectiveAlpha() {
    this->CSimpleRegion::UpdateEffectiveAlpha();
    uint32_t color = 0xffffffff;
    if ((this->m_regionFlags & REGIONFLAG_GRADIENT) == 0) {
        color = this->m_color;
    }
    if (this->m_textBlock != nullptr) {
        TextBlockUpdateColor(this->m_textBlock, &color);
        if ((this->m_stringFlags & FONTSTRING_SHADOW) != 0) {
            TextBlockSetShadow(this->m_textBlock, ShadowColorWithAlpha(this), this->m_shadowOffset);
        }
    }
    uint8_t alpha = static_cast<uint8_t>(color >> 24);
    for (CSimpleEmbeddedTexture* node = this->m_embeddedTextures.Head(); node != nullptr;
         node = this->m_embeddedTextures.Link(node)->Next()) {
        if (node->m_texture != nullptr && node->m_texture->m_parent != nullptr) {
            node->m_texture->SetAlpha(alpha);
        }
    }
}

// ref: FUN_140623d10
void CSimpleFontString::AddToBatch(CRenderBatch* batch) {
    if (!this->m_font || this->m_text == nullptr || this->m_text[0] == '\0') {
        return;
    }
    RenderBatchAddFontString(batch, this);
    for (CSimpleEmbeddedTexture* node = this->m_embeddedTextures.Head(); node != nullptr;
         node = this->m_embeddedTextures.Link(node)->Next()) {
        if (node->m_texture != nullptr && node->m_texture->m_texture != nullptr) {
            RenderBatchAddTexture(batch, node->m_texture, reinterpret_cast<CSimpleTexture*>(this));
        }
    }
}

// ref: FUN_1406286a0
void CSimpleFontString::SetDrawLayer(uint8_t layer, int8_t subLevel) {
    (void)subLevel;
    this->CSimpleRegion::SetDrawLayer(layer, this->m_subLevel);
}

// The draw order key of the string's layer, sub-level 8.
// ref: FUN_140625070
uint64_t CSimpleFontString::GetOwnDrawOrderKey() {
    if (this->m_parent == nullptr) {
        return 0;
    }
    return this->m_parent->GetDrawOrderKey(this->m_drawLayer, 8);
}

// The animation-free rotation and scale are kept, and the anchor taken as the anim origin.
// ref: FUN_140629070
void CSimpleFontString::Unknown2C0() {
    this->m_baseRotation = this->m_rotation;
    this->m_baseTextScale = this->m_textScale;
    ANIMTRANSFORM transform = {};
    transform.m_values[0] = this->m_rect.minX;
    transform.m_values[1] = this->m_rect.minY;
    transform.m_values[2] = this->m_layoutDepth;
    this->SetAnimTransform(&transform);
}

// ---- CLayoutFrame --------------------------------------------------------------------------------

// ref: FUN_140627710
void CSimpleFontString::ScaleBy(float* scale) {
    this->m_fontHeight *= *scale;
    this->CLayoutFrame::ScaleBy(scale);
}

// A new scale remakes the font at it.
// ref: FUN_140628950
bool CSimpleFontString::SetLayoutScale(float scale, bool force) {
    if (!this->CSimpleRegion::SetLayoutScale(scale, force)) {
        return false;
    }
    if (!this->m_font) {
        return true;
    }
    if ((TextBlockGetFontFlags(this->m_font) & TEXTBLOCK_FONT_FIXED_SIZE) != 0) {
        this->m_stringWidth = 0.0f;
        this->m_stringHeight = 0.0f;
        return true;
    }
    uint16_t attributeFlags = this->m_fontObject != nullptr ? this->m_fontObject->m_attributes.m_flags : 0;
    const char* name = this->m_font ? TextBlockGetFontName(this->m_font) : nullptr;
    uint8_t flags = this->m_font ? TextBlockGetFontFlags(this->m_font) : 0;
    if (name == nullptr || *name == '\0') {
        int32_t fileDataId = this->m_font ? TextBlockGetFontFileDataId(this->m_font) : 0;
        this->SetFont(fileDataId, this->m_fontHeight, flags, attributeFlags);
    } else {
        this->SetFont(name, this->m_fontHeight, flags, attributeFlags);
    }
    return true;
}

// ref: FUN_140629030
void CSimpleFontString::SetWidth(float width) {
    this->CLayoutFrame::SetWidth(width);
    this->m_stringWidth = 0.0f;
    this->m_stringHeight = 0.0f;
}

// ref: FUN_140628930
void CSimpleFontString::SetHeight(float height) {
    this->CLayoutFrame::SetHeight(height);
    this->m_stringWidth = 0.0f;
    this->m_stringHeight = 0.0f;
}

// The set width, the anchors' span, or the text's own width; at least one texel.
// ref: FUN_140626270
float CSimpleFontString::GetWidth() {
    float width = this->CLayoutFrame::GetWidth();
    if (width == 0.0f) {
        float left = this->CalculateLeft();
        float right = this->CalculateRight();
        if (left == LAYOUT_UNSET || right == LAYOUT_UNSET || (width = (right - left) / this->m_layoutScale) == 0.0f) {
            width = this->GetStringWidth();
        }
    }
    float minimum = (1.0f / (g_coordinateSpace.m_aspectCompensation * 1024.0f)) * g_coordinateSpace.m_diagonalWidth;
    return width <= minimum ? minimum : width;
}

// ref: FUN_140625270
float CSimpleFontString::GetHeight() {
    float height = this->CLayoutFrame::GetHeight();
    if (height == 0.0f) {
        height = this->GetStringHeight();
    }
    float minimum = (1.0f / (g_coordinateSpace.m_aspectCompensation * 1024.0f)) * g_coordinateSpace.m_diagonalWidth;
    return minimum < height ? height : minimum;
}

// Ready when every embedded texture is.
// ref: FUN_140626c50
bool CSimpleFontString::IsLayoutReady() {
    for (CSimpleEmbeddedTexture* node = this->m_embeddedTextures.Head(); node != nullptr;
         node = this->m_embeddedTextures.Link(node)->Next()) {
        if (!node->m_texture->IsLayoutReady()) {
            return false;
        }
    }
    return true;
}

// ref: FUN_140628020
void CSimpleFontString::OnLayoutReady() {
    if (this->m_parent != nullptr) {
        this->m_parent->MarkBatchDirty();
    }
}

// ref: FUN_140627fd0
void CSimpleFontString::OnLayoutDepthChanged() {
    if (this->m_textBlock != nullptr) {
        float position[3] = { this->m_rect.minX, this->m_rect.minY, this->m_layoutDepth };
        TextBlockSetStringPos(this->m_textBlock, position);
    }
}

// A move keeps the text block and moves it; a new size rebuilds it.
// ref: FUN_140627740
void CSimpleFontString::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->m_stringWidth = 0.0f;
    this->m_stringHeight = 0.0f;
    this->CLayoutFrame::OnFrameSizeChanged(rect, previous);
    bool rebuild = true;
    if (this->m_textBlock != nullptr
        && Abs((previous->maxX - previous->minX) - (rect->maxX - rect->minX)) < SIZE_EPSILON
        && Abs((previous->maxY - previous->minY) - (rect->maxY - rect->minY)) < SIZE_EPSILON) {
        rebuild = false;
        float position[3] = { rect->minX, rect->minY, this->m_layoutDepth };
        TextBlockSetStringPos(this->m_textBlock, position);
        auto offscreen = [](const CRect* r) {
            return r->maxY < 0.0f || 1.0f < r->minY || r->maxX < 0.0f || 1.0f < r->minX;
        };
        if (offscreen(previous) != offscreen(rect)) {
            this->MarkFrameBatchDirty();
        }
    }
    if (rebuild) {
        this->UpdateString();
    }
    if (this->HasAnimTransform()) {
        float* origin = this->m_animData != nullptr ? this->m_animData->m_transform.m_values : nullptr;
        if (origin != nullptr) {
            ANIMTRANSFORM transform = {};
            transform.m_values[0] = rect->minX;
            transform.m_values[1] = rect->minY;
            transform.m_values[2] = this->m_layoutDepth;
            if (this->m_textBlock != nullptr) {
                float current[3];
                TextBlockGetStringPos(current, this->m_textBlock);
                float moved[3] = {
                    (rect->minX - origin[0]) + current[0],
                    (rect->minY - origin[1]) + current[1],
                    (this->m_layoutDepth - origin[2]) + current[2],
                };
                TextBlockSetStringPos(this->m_textBlock, moved);
            }
            this->SetAnimTransform(&transform);
        }
    }
    if (this->m_parent != nullptr) {
        this->m_parent->MarkScrollRangeDirty();
    }
}

// The rotation and scale lose the animation's part.
// ref: FUN_1406232a0
void CSimpleFontString::ClearAnimTransform() {
    this->m_rotation = this->m_baseRotation;
    this->m_textScale = this->m_baseTextScale;
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    if (this->m_textBlock != nullptr && this->m_textBlock->m_string != nullptr) {
        CGxString* string = this->m_textBlock->m_string;
        GxuFontStringSetRotation(string, this->m_baseRotation);
        GxuFontStringSetScale(string, this->m_baseTextScale);
        string->ClearGeometry();
    }
    if (this->HasAnimTransform() && this->m_textBlock != nullptr && this->m_animData != nullptr) {
        TextBlockSetStringPos(this->m_textBlock, this->m_animData->m_transform.m_values);
    }
    this->CLayoutFrame::ClearAnimTransform();
}

// ---- CAnimatableObject ---------------------------------------------------------------------------

// The animation's transform goes to the text block.
// ref: FUN_140628130
void CSimpleFontString::Unknown08() {
    if (this->HasAnimTransform()) {
        if (this->m_textBlock != nullptr && this->m_animData != nullptr) {
            TextBlockSetStringPos(this->m_textBlock, this->m_animData->m_transform.m_values);
        }
        if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
            this->Resize(true);
        }
        if (this->m_textBlock != nullptr && this->m_textBlock->m_string != nullptr) {
            CGxString* string = this->m_textBlock->m_string;
            GxuFontStringSetRotation(string, this->m_baseRotation);
            GxuFontStringSetScale(string, this->m_baseTextScale);
            string->ClearGeometry();
        }
    }
    if (this->HasAnimAlpha()) {
        this->SetAlpha(this->GetAnimAlpha());
    }
}

// A translation moves the text block by the offset at the layout's scale.
// ref: FUN_140623050
void CSimpleFontString::Unknown40(void* value) {
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    if (this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return;
    }
    auto offset = static_cast<const float*>(value);
    float position[3];
    TextBlockGetStringPos(position, this->m_textBlock);
    position[0] += this->m_layoutScale * offset[0];
    position[1] += this->m_layoutScale * offset[1];
    TextBlockSetStringPos(this->m_textBlock, position);
    this->m_textBlock->m_string->ClearGeometry();
}

// A rotation turns the text from its resting rotation.
// ref: FUN_140622f50
void CSimpleFontString::Unknown58(int32_t index, void* value, float radians) {
    (void)index;
    (void)value;
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    if (this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return;
    }
    this->m_rotation = radians + this->m_baseRotation;
    GxuFontStringSetRotation(this->m_textBlock->m_string, this->m_rotation);
    this->m_textBlock->m_string->ClearGeometry();
}

// A scale shrinks the text from its resting scale.
// ref: FUN_140622fd0
void CSimpleFontString::Unknown60(int32_t index, void* first, void* second) {
    (void)index;
    (void)first;
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    if (this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return;
    }
    this->m_textScale = (1.0f - *static_cast<const float*>(second)) * this->m_baseTextScale;
    GxuFontStringSetScale(this->m_textBlock->m_string, this->m_textScale);
    this->m_textBlock->m_string->ClearGeometry();
}

// ref: FUN_140628630
void CSimpleFontString::SetAnimAlpha(uint8_t alpha) {
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    if (this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return;
    }
    this->CSimpleRegion::SetAnimAlpha(alpha);
    this->m_textBlock->m_string->ClearGeometry();
}

// ---- CSimpleFontable -----------------------------------------------------------------------------

// A new font object puts a standing-in member back first, and picks a member for the text after.
// ref: FUN_140628790
void CSimpleFontString::SetFontObject(CSimpleFont* font) {
    if (font != nullptr && this->m_familyFontObject != nullptr) {
        this->RestoreFamily();
        this->CSimpleFontable::SetFontObject(font);
        if (this->m_text != nullptr && this->m_text[0] != '\0') {
            this->UpdateFamilyMember(this->GetDisplayText(), false, false);
        }
        return;
    }
    this->CSimpleFontable::SetFontObject(font);
}

// What changed in the font object, where the string still takes it from there.
// ref: FUN_1406292b0
void CSimpleFontString::FontObjectUpdated(CSimpleFontAttributes* attributes) {
    uint16_t flags = attributes->m_flags;
    uint16_t mask = flags & this->m_fontableFlags;
    if ((flags & CSimpleFontAttributes::HAS_FONT) != 0 && (mask & CSimpleFontAttributes::CHANGED_FONT) != 0) {
        this->SetFont(attributes->m_fontPath, attributes->m_height, attributes->m_fontFlags, flags);
    }
    if ((attributes->m_flags & CSimpleFontAttributes::HAS_FLAGS) != 0 && (mask & CSimpleFontAttributes::CHANGED_FLAGS) != 0) {
        uint32_t stringFlags = (~this->m_inheritedFlags & this->m_stringFlags) | (attributes->m_stringFlags & this->m_inheritedFlags);
        if (this->m_stringFlags != stringFlags) {
            this->m_stringFlags = stringFlags;
            if (this->m_textBlock != nullptr) {
                this->UpdateString();
            }
        }
    }
    if ((attributes->m_flags & CSimpleFontAttributes::HAS_COLOR) != 0 && (mask & CSimpleFontAttributes::CHANGED_COLOR) != 0) {
        this->CSimpleRegion::SetVertexColor(&attributes->m_color);
    }
    if ((attributes->m_flags & CSimpleFontAttributes::HAS_SHADOW) == 0) {
        if ((this->m_stringFlags & FONTSTRING_SHADOW) != 0) {
            this->m_stringFlags &= ~FONTSTRING_SHADOW;
            if (this->m_textBlock != nullptr) {
                TextBlockClearShadow(this->m_textBlock);
            }
        }
    } else if ((mask & CSimpleFontAttributes::CHANGED_SHADOW) != 0) {
        if (attributes->m_shadowOffset[0] == 0.0f && attributes->m_shadowOffset[1] == 0.0f) {
            this->ClearShadow();
        } else {
            this->SetShadow(&attributes->m_shadowColor, attributes->m_shadowOffset);
        }
    }
    if ((attributes->m_flags & CSimpleFontAttributes::HAS_SPACING) != 0 && (mask & CSimpleFontAttributes::CHANGED_SPACING) != 0) {
        float spacing = attributes->m_spacing;
        if (spacing <= 0.0f) {
            spacing = 0.0f;
        }
        if (EPSILON <= Abs(spacing - this->m_spacing)) {
            this->m_spacing = spacing;
            this->m_stringHeight = 0.0f;
            if (this->m_textBlock != nullptr) {
                this->UpdateString();
            }
        }
    }
}

// ---- the font ------------------------------------------------------------------------------------

namespace {

// The shared tail of the two SetFont paths: the new font replaces the old one, which is kept
// for a frame when asked; a different font drops the text block and asks for a layout.
bool TakeFont(CSimpleFontString* string, HTEXTFONT&& font, float height, uint16_t attributeFlags) {
    bool changed = font != string->m_font || CSimpleTop::s_instance->m_resizing;
    string->m_fontHeight = height;
    if (string->m_textBlock != nullptr && changed) {
        HandleClose(string->m_textBlock);
        string->m_textBlock = nullptr;
    }
    if (string->m_font && (attributeFlags & CSimpleFontAttributes::RESTORING) != 0) {
        UiAddUnneededFontHandle(&string->m_font);
    }
    string->m_font = static_cast<HTEXTFONT&&>(font);
    if (string->m_font && changed) {
        string->m_stringWidth = 0.0f;
        string->m_stringHeight = 0.0f;
        if (string->m_textBlock != nullptr) {
            HandleClose(string->m_textBlock);
            string->m_textBlock = nullptr;
        }
        if (string->CLayoutFrame::GetWidth() == 0.0f || string->CLayoutFrame::GetHeight() == 0.0f) {
            string->ClearRectFailed();
        }
        string->Resize(false);
    }
    return true;
}

}

// ref: FUN_140622410
bool CSimpleFontString::SetFont(int32_t fileDataId, float height, uint8_t fontFlags, uint16_t attributeFlags) {
    HTEXTFONT font;
    if (fileDataId != 0 && 0.0f < height) {
        float scaled = height;
        if ((fontFlags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
            scaled = height * this->m_layoutScale;
        }
        font = TextBlockGenerateFont(fileDataId, fontFlags, scaled * this->m_fontScale);
        if (!font) {
            return false;
        }
    }
    return TakeFont(this, static_cast<HTEXTFONT&&>(font), height, attributeFlags);
}

// ref: FUN_140622660
bool CSimpleFontString::SetFont(const char* path, float height, uint8_t fontFlags, uint16_t attributeFlags) {
    HTEXTFONT font;
    if (path != nullptr && 0.0f < height) {
        float scaled = height;
        if ((fontFlags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
            scaled = height * this->m_layoutScale;
        }
        font = TextBlockGenerateFont(path, fontFlags, scaled * this->m_fontScale);
        if (!font) {
            return false;
        }
    }
    return TakeFont(this, static_cast<HTEXTFONT&&>(font), height, attributeFlags);
}

// ref: FUN_1406260b0
uint8_t CSimpleFontString::GetFontFlags() {
    return this->m_font ? TextBlockGetFontFlags(this->m_font) : 0;
}

// ref: FUN_1406251d0
int32_t CSimpleFontString::GetFontFileDataId() {
    return this->m_font ? GxuFontGetFileDataId(this->m_font->m_font) : 0;
}

// ref: FUN_140625250
const char* CSimpleFontString::GetFontName() {
    return this->m_font ? GxuFontGetFontName(this->m_font->m_font) : nullptr;
}

// ref: FUN_1406251f0
float CSimpleFontString::GetFontHeight(bool oneToOne) {
    if (oneToOne && this->m_font && (this->m_stringFlags & FONTSTRING_ONE_TO_ONE) != 0) {
        float pixels = GxuFontGetOneToOneHeight(TextBlockGetFontPtr(this->m_font));
        return (pixels / this->m_layoutScale) * g_coordinateSpace.m_diagonalHeight;
    }
    return this->m_fontHeight;
}

// ref: FUN_140623460
float CSimpleFontString::GetLineHeight() {
    return SnapToPixels(this->GetFontHeight(true) + this->m_spacing);
}

// ref: FUN_1406234d0
float CSimpleFontString::GetScaledLineHeight() {
    return (this->GetFontHeight(true) + this->m_spacing) * this->m_layoutScale;
}

// ref: FUN_140625870
float CSimpleFontString::GetEffectiveFontHeight() {
    if (!this->m_font) {
        return 0.0f;
    }
    float height = this->GetFontHeight(true);
    return TextBlockGetFontHeight(this->m_font, height * this->m_layoutScale, this->m_stringFlags) / this->m_layoutScale;
}

// ref: FUN_140628e70
void CSimpleFontString::SetFontHeight(float height) {
    if (height <= 0.0f) {
        FrameScript_ActionError("Invalid texHeight: %f, height must be > 0", static_cast<double>(height));
        return;
    }
    if (EPSILON <= Abs(height - this->m_fontHeight)) {
        this->m_stringFlags &= ~FONTSTRING_ONE_TO_ONE;
        this->m_fontHeight = height;
        this->ClearString();
    }
}

// ref: FUN_140628f80
void CSimpleFontString::SetFontScale(float scale) {
    if (scale <= 1.1920929e-07f) {
        FrameScript_ActionError("Invalid scale: %f, scale must be > 0", static_cast<double>(scale));
        return;
    }
    if (EPSILON <= Abs(this->m_fontScale - scale)) {
        this->m_fontScale = scale;
        this->SetLayoutScale(this->m_layoutScale, true);
    }
}

// ---- text ----------------------------------------------------------------------------------------

// ref: FUN_140628cc0
void CSimpleFontString::SetText(const char* text, bool process) {
    if (text == nullptr || (process && (text = FontStringProcessGrammar(text)) == nullptr) || *text == '\0') {
        if (this->m_text == nullptr || this->m_text[0] == '\0') {
            return;
        }
    }
    if (text != nullptr) {
        char* current = this->m_text;
        if (current != nullptr && std::strcmp(text, current) == 0) {
            return;
        }
        if (*text != '\0') {
            uint32_t sizes = static_cast<uint32_t>(this->m_maxBytes) | (static_cast<uint32_t>(this->m_textCapacity) << 16);
            if (this->m_maxBytes == 0 || current == nullptr) {
                uint64_t length = std::strlen(text);
                if ((sizes >> 16) < length) {
                    SMemFree(current, s_sourceFile, 0x2f1, 0);
                    this->m_text = SStrDupA(text, nullptr, 0);
                    this->m_textCapacity = static_cast<uint16_t>(length);
                } else {
                    std::strcpy(current, text);
                }
            } else {
                CopyBounded(current, text, this->m_maxBytes);
            }
            goto done;
        }
    }
    if (this->m_text != nullptr) {
        this->m_text[0] = '\0';
    }
done:
    this->m_textFlags &= ~TEXT_PROCESSED;
    if ((this->m_textFlags & TEXT_FAMILY_CHECKED) == 0) {
        this->UpdateFamilyMember(this->GetDisplayText(), process, false);
    }
    this->ClearString();
}

// ref: FUN_140628fe0
void CSimpleFontString::SetTextChecked(const char* text) {
    if (!this->m_font && this->m_fontRequest == nullptr) {
        FrameScript_ActionError("Font not set");
        return;
    }
    this->SetText(text, true);
}

// ref: FUN_140625c30
const char* CSimpleFontString::GetDisplayText() {
    const char* text = this->m_text;
    if (text == nullptr || *text == '\0') {
        text = nullptr;
    }
    if ((this->m_textFlags & TEXT_PROCESSED) == 0 || (this->m_textFlags & TEXT_EXPANDED) != 0) {
        if (FontStringExpandCodes(text, reinterpret_cast<blz::string_header*>(&s_expandedText))) {
            this->m_textFlags |= TEXT_EXPANDED;
            text = s_expandedText.m_data;
        } else {
            this->m_textFlags &= ~TEXT_EXPANDED;
        }
        this->m_textFlags |= TEXT_PROCESSED;
    }
    return text;
}

// ref: FUN_140624d40
const char* CSimpleFontString::GetFittedText(float width, float height, bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    const char* text = this->GetDisplayText();
    uint32_t maxLines = this->m_maxLines & 0xffffff;
    if (!this->m_font || text == nullptr || *text == '\0' || !(0.0f < width) || (maxLines == 0 && !(0.0f < height))) {
        return text;
    }
    float fontHeight = this->GetFontHeight(true);
    float scale = this->m_layoutScale;
    float lineHeight = (SnapToPixels(this->m_spacing) + fontHeight) * scale;
    float maxHeight;
    if (maxLines == 0) {
        maxHeight = lineHeight <= height ? height : lineHeight;
    } else {
        maxHeight = static_cast<float>(maxLines) * lineHeight;
    }
    uint32_t length = static_cast<uint32_t>(std::strlen(text));
    uint32_t fits = TextBlockGetMaxBytesWithinHeight(this->m_font, text, scale * fontHeight, width, maxHeight,
        static_cast<int32_t>(length), this->m_shadowOffset[0], scale, scale * this->m_spacing, this->m_stringFlags);
    if (length <= fits) {
        return text;
    }
    if (0x1ffb < length) {
        length = 0x1ffc;
    }
    CopyBounded(s_fittedText, text, sizeof(s_fittedText));
    do {
        GxuFontBackUpOverCode(s_fittedText, &length);
        if (length != 0 && s_fittedText[length - 1] == ' ') {
            length--;
        }
        std::memcpy(s_fittedText + length, "...", 4);
        if (truncated != nullptr) {
            *truncated = true;
        }
        scale = this->m_layoutScale;
        fits = TextBlockGetMaxBytesWithinHeight(this->m_font, s_fittedText, scale * fontHeight, width, maxHeight,
            static_cast<int32_t>(length + 3), this->m_shadowOffset[0], scale, scale * this->m_spacing, this->m_stringFlags);
    } while (length != 0 && fits < length + 3);
    return s_fittedText;
}

// ref: FUN_140626d00
bool CSimpleFontString::IsTruncated() {
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    bool truncated = false;
    float height = this->GetLayoutHeight();
    float width = this->GetLayoutWidth();
    this->GetFittedText(width, height, &truncated);
    return truncated;
}

// ref: FUN_140628ec0
void CSimpleFontString::SetMaxBytes(uint32_t bytes) {
    if (bytes == this->m_maxBytes) {
        return;
    }
    if (bytes != 0) {
        auto text = static_cast<char*>(SMemAlloc(bytes + 1, 0x1f, s_sourceFile, 0, 0x281, 0));
        *text = '\0';
        SMemFree(this->m_text, s_sourceFile, 0x284, 0);
        this->m_text = text;
        this->m_textCapacity = static_cast<uint16_t>(bytes);
    }
    this->m_maxBytes = static_cast<uint16_t>(bytes);
}

// ref: FUN_140629b20
uint32_t CSimpleFontString::GetLineStarts(const char* text, float width, int32_t* offsets, uint32_t maxOffsets) {
    if (!this->m_font || text == nullptr) {
        return 0;
    }
    float scale = this->m_layoutScale;
    float height = this->GetFontHeight(true);
    return TextBlockGetLineStarts(this->m_font, text, height * scale, scale * width, offsets, maxOffsets,
        this->m_shadowOffset[0], scale, 0.0f, this->m_stringFlags);
}

// ref: FUN_140625b80
uint32_t CSimpleFontString::GetNumLines() {
    int32_t offsets[0x400];
    const char* text = this->GetDisplayText();
    uint32_t count = this->GetLineStarts(text, this->GetWidth(), offsets, 0x400);
    if (count < 0x400) {
        return this->GetLineStarts(text, this->GetWidth(), offsets, 0x400);
    }
    return 0x400;
}

// ref: FUN_140625f10
float CSimpleFontString::GetStringWidth() {
    if (this->m_stringWidth == 0.0f && this->m_font) {
        float scale = this->m_layoutScale;
        const char* text = this->GetFittedText(this->CLayoutFrame::GetWidth() * scale, this->CLayoutFrame::GetHeight() * scale, nullptr);
        if (text != nullptr && *text != '\0') {
            float height = this->GetFontHeight(true);
            TextBlockGetWrappedWidth(this->m_font, text, static_cast<uint32_t>(std::strlen(text)), height * this->m_layoutScale,
                &this->m_stringWidth, this->m_shadowOffset[0], this->m_layoutScale, 0.0f, this->m_stringFlags);
        }
    }
    return this->m_stringWidth / this->m_layoutScale;
}

// ref: FUN_140625d60
float CSimpleFontString::GetStringHeight() {
    if (this->m_stringHeight == 0.0f && this->m_font) {
        float scale = this->m_layoutScale;
        const char* text = this->GetFittedText(this->CLayoutFrame::GetWidth() * scale, this->CLayoutFrame::GetHeight() * scale, nullptr);
        if (text != nullptr && *text != '\0') {
            float spacing = this->m_spacing;
            float width = this->GetWidth();
            float height = this->GetFontHeight(true);
            this->m_stringHeight = TextBlockGetWrappedTextHeight(this->m_font, text, height * this->m_layoutScale,
                width * this->m_layoutScale, this->m_shadowOffset, this->m_layoutScale, this->m_layoutScale * spacing,
                this->m_stringFlags);
        }
    }
    return this->m_stringHeight / this->m_layoutScale;
}

// The widest of the lines the text breaks into at the current width.
// ref: FUN_140626520
float CSimpleFontString::GetUnboundedStringWidth() {
    int32_t offsets[0x400];
    float width = this->GetWidth();
    const char* text = this->GetDisplayText();
    uint32_t count = this->GetLineStarts(text, width, offsets, 0x400);
    if (count < 0x400) {
        count = this->GetLineStarts(text, width, offsets, 0x400);
        if (count == 0) {
            return 0.0f;
        }
    } else {
        count = 0x400;
    }
    float widest = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t end = i < count - 1 ? static_cast<uint32_t>(offsets[i + 1]) : static_cast<uint32_t>(std::strlen(text));
        float line = this->GetTextWidth(text + offsets[i], end - static_cast<uint32_t>(offsets[i]));
        if (widest < line) {
            widest = line;
        }
    }
    return widest;
}

// ref: FUN_1406260f0
float CSimpleFontString::GetTextWidth(const char* text, uint32_t length) {
    if (text == nullptr) {
        text = this->m_text;
        if (text == nullptr || *text == '\0') {
            return 0.0f;
        }
    }
    if (!this->m_font) {
        return 0.0f;
    }
    if (length == 0) {
        length = static_cast<uint32_t>(std::strlen(text));
    }
    float width = 0.0f;
    float height = this->GetFontHeight(true);
    TextBlockGetWrappedWidth(this->m_font, text, length, height * this->m_layoutScale, &width, this->m_shadowOffset[0],
        this->m_layoutScale, 0.0f, this->m_stringFlags);
    return width / this->m_layoutScale;
}

// A lone newline measures as a space.
// ref: FUN_140623420
float CSimpleFontString::GetRangeWidth(const char* text, uint32_t start, uint32_t end) {
    if (start != end && (end - start != 1 || text[start] != '\n')) {
        return this->GetTextWidth(text + start, end - start);
    }
    return this->GetTextWidth(" ", 1);
}

// ref: FUN_1406250a0
float CSimpleFontString::GetWrappedWidth(const char* text) {
    if (!this->m_font) {
        return 0.0f;
    }
    float width = 0.0f;
    float height = this->GetFontHeight(true);
    TextBlockGetWrappedWidth(this->m_font, text, 0, height * this->m_layoutScale, &width, this->m_shadowOffset[0],
        this->m_layoutScale, 0.0f, this->m_stringFlags);
    return width / this->m_layoutScale;
}

// ref: FUN_140625920
uint32_t CSimpleFontString::GetTextExtentCount(const char* text, uint32_t length, float width) {
    if (!this->m_font) {
        return 0;
    }
    if (length == 0) {
        length = static_cast<uint32_t>(std::strlen(text));
    }
    float extent = 0.0f;
    float scale = this->m_layoutScale;
    float height = this->GetFontHeight(true);
    return static_cast<uint32_t>(TextBlockGetTextExtent(this->m_font, text, height * scale, scale * width, length, &extent,
        this->m_shadowOffset[0], scale, 0.0f, this->m_stringFlags));
}

// ref: FUN_140625a50
uint32_t CSimpleFontString::GetTrailingCharCount(const char* text, uint32_t length, float width) {
    if (!this->m_font) {
        return 0;
    }
    if (length == 0) {
        length = static_cast<uint32_t>(std::strlen(text));
    }
    float extent = 0.0f;
    float scale = this->m_layoutScale;
    float height = this->GetFontHeight(true);
    return TextBlockGetMaxCharsWithinWidth(this->m_font, text, height * scale, scale * width, length, &extent,
        this->m_shadowOffset[0], scale, 0.0f, this->m_stringFlags);
}

// ref: FUN_1406264b0
float CSimpleFontString::GetLayoutWidth() {
    float width = this->CLayoutFrame::GetWidth();
    if (width == 0.0f) {
        width = this->m_stringWidth;
    } else {
        width *= this->m_layoutScale;
    }
    if ((this->CLayoutFrame::m_flags & FLAG_RECT_VALID) != 0) {
        float span = this->m_rect.maxX - this->m_rect.minX;
        if (SIZE_EPSILON <= Abs(width - span)) {
            width = span;
        }
    }
    return width;
}

// ref: FUN_1406252d0
float CSimpleFontString::GetLayoutHeight() {
    float height = this->CLayoutFrame::GetHeight();
    if (height == 0.0f) {
        height = this->m_stringHeight;
    } else {
        height *= this->m_layoutScale;
    }
    if ((this->CLayoutFrame::m_flags & FLAG_RECT_VALID) != 0) {
        float span = this->m_rect.maxY - this->m_rect.minY;
        if (SIZE_EPSILON <= Abs(height - span)) {
            height = span;
        }
    }
    return height;
}

// ---- appearance ----------------------------------------------------------------------------------

// ref: FUN_140623100
void CSimpleFontString::SetShadow(const uint32_t* color, const float* offset) {
    if ((this->m_stringFlags & FONTSTRING_SHADOW) != 0 && *color == this->m_shadowColor && this->m_shadowOffset[0] == offset[0]
        && this->m_shadowOffset[1] == offset[1]) {
        return;
    }
    this->m_stringFlags |= FONTSTRING_SHADOW;
    this->m_shadowColor = *color;
    this->m_shadowOffset[0] = offset[0];
    this->m_shadowOffset[1] = offset[1];
    if (this->m_textBlock != nullptr) {
        TextBlockSetShadow(this->m_textBlock, ShadowColorWithAlpha(this), this->m_shadowOffset);
    }
}

// ref: FUN_140628450
void CSimpleFontString::ClearShadow() {
    if ((this->m_stringFlags & FONTSTRING_SHADOW) == 0) {
        return;
    }
    this->m_stringFlags &= ~FONTSTRING_SHADOW;
    if (this->m_textBlock != nullptr) {
        TextBlockClearShadow(this->m_textBlock);
    }
}

// ref: FUN_140628b30
void CSimpleFontString::SetShadowColor(uint32_t* color, const uint8_t* alpha) {
    if (alpha[1] != 0) {
        *color = (*color & 0xffffff) | (static_cast<uint32_t>(alpha[0]) << 24);
    }
    if (*color != this->m_shadowColor) {
        this->m_shadowColor = *color;
        if (this->m_textBlock != nullptr) {
            this->UpdateString();
        }
    }
    this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
}

// ref: FUN_140628b80
void CSimpleFontString::SetShadowOffset(const float* offset) {
    if (this->m_shadowOffset[0] != offset[0] || this->m_shadowOffset[1] != offset[1]) {
        this->m_shadowOffset[0] = offset[0];
        this->m_shadowOffset[1] = offset[1];
        if (offset[0] == 0.0f && this->m_shadowOffset[1] == 0.0f) {
            this->ClearShadow();
        } else {
            uint32_t color = this->m_shadowColor;
            this->SetShadow(&color, this->m_shadowOffset);
        }
        this->ClearString();
    }
    this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
}

// ref: FUN_140628c10
void CSimpleFontString::SetSpacing(float spacing) {
    if (spacing <= 0.0f) {
        spacing = 0.0f;
    }
    if (EPSILON <= Abs(spacing - this->m_spacing)) {
        this->m_spacing = spacing;
        this->m_stringHeight = 0.0f;
        if (this->m_textBlock != nullptr) {
            this->UpdateString();
        }
    }
}

// ref: FUN_140628c60
void CSimpleFontString::SetSpacingFromFont(float spacing) {
    this->SetSpacing(spacing);
    this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SPACING;
}

// ref: FUN_140628b10
void CSimpleFontString::SetRotation(float radians) {
    this->m_rotation = radians;
    if (this->m_textBlock != nullptr) {
        this->UpdateString();
    }
}

// ref: FUN_1406285e0
bool CSimpleFontString::SetAlphaGradient(int16_t start, int32_t length) {
    if (length < 1) {
        FrameScript_ActionError("length must be greater than 0");
        return false;
    }
    this->m_gradientStart = start;
    this->m_gradientLength = static_cast<int16_t>(length);
    if (this->m_textBlock == nullptr) {
        return true;
    }
    return TextBlockSetGradient(this->m_textBlock, start, length) < 2;
}

void CSimpleFontString::SetJustifyH(uint32_t justify) {
    uint32_t flags = (this->m_stringFlags & ~FONTSTRING_JUSTIFY_H) | (justify & FONTSTRING_JUSTIFY_H);
    if (this->m_stringFlags != flags) {
        this->m_stringFlags = flags;
        if (this->m_textBlock != nullptr) {
            this->UpdateString();
        }
    }
}

void CSimpleFontString::SetJustifyV(uint32_t justify) {
    uint32_t flags = (this->m_stringFlags & ~FONTSTRING_JUSTIFY_V) | (justify & FONTSTRING_JUSTIFY_V);
    if (this->m_stringFlags != flags) {
        this->m_stringFlags = flags;
        if (this->m_textBlock != nullptr) {
            this->UpdateString();
        }
    }
}

void CSimpleFontString::SetStringFlag(uint32_t flag, bool on) {
    uint32_t flags = on ? (this->m_stringFlags | flag) : (this->m_stringFlags & ~flag);
    if (this->m_stringFlags != flags) {
        this->m_stringFlags = flags;
        if (this->m_textBlock != nullptr) {
            this->UpdateString();
        }
    }
}

// ---- the text block ------------------------------------------------------------------------------

// ref: FUN_140628480
void CSimpleFontString::ClearString() {
    this->m_stringWidth = 0.0f;
    this->m_stringHeight = 0.0f;
    if (this->m_textBlock != nullptr) {
        HandleClose(this->m_textBlock);
        this->m_textBlock = nullptr;
    }
    if (this->CLayoutFrame::GetWidth() == 0.0f || this->CLayoutFrame::GetHeight() == 0.0f) {
        this->ClearRectFailed();
    }
    this->Resize(false);
}

// Takes a font asked for earlier, drops the old block and its textures, and lays the text out
// again in the rect: the fitted text, the colour, shadow and gradient, the embedded textures
// and the animation's origin.
// ref: FUN_140629430
void CSimpleFontString::UpdateString() {
    if ((this->CLayoutFrame::m_flags & FLAG_RECT_VALID) == 0) {
        return;
    }
    if (this->m_fontRequest != nullptr) {
        FONTSTRINGREQUEST* request = this->m_fontRequest;
        if (request->m_fileDataId == 0) {
            this->SetFont(request->m_path, request->m_height, request->m_fontFlags, 0);
        } else {
            this->SetFont(request->m_fileDataId, request->m_height, request->m_fontFlags, 0);
        }
        operator delete(this->m_fontRequest);
        this->m_fontRequest = nullptr;
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FONT;
    }
    if (this->m_textBlock != nullptr) {
        HandleClose(this->m_textBlock);
        this->m_textBlock = nullptr;
    }
    bool hadTextures = this->m_embeddedTextures.Head() != nullptr;
    this->ReleaseEmbeddedTextures();

    if (!(this->m_rect.minY < this->m_rect.maxY) || !(this->m_rect.minX < this->m_rect.maxX)) {
        return;
    }

    if (this->m_font && this->m_text != nullptr && this->m_text[0] != '\0') {
        float position[3] = { this->m_rect.minX, this->m_rect.minY, this->m_layoutDepth };
        float width = this->GetLayoutWidth();
        float height = this->GetLayoutHeight();
        const char* text = this->GetFittedText(width, height, nullptr);
        uint32_t flags = this->m_stringFlags;
        if ((flags & FONTSTRING_NO_COLOR_CODES) == 0) {
            bool codes = false;
            for (uint64_t i = 0; text[i] != '\0'; i++) {
                if (text[i] == '|') {
                    if (text[i + 1] == '|') {
                        i++;
                    } else if (((text[i + 1] + 0xbdu) & 0xdf) == 0) {
                        codes = true;
                        break;
                    }
                }
            }
            if (!codes) {
                flags |= FONTSTRING_NO_COLOR_CODES;
            }
        }
        uint32_t color = RegionColor(this);
        float scale = this->m_layoutScale;
        float fontHeight = this->GetFontHeight(true);
        this->m_textBlock = TextBlockCreate(this->m_font, text, &color, position, fontHeight * scale, width, height, flags,
            0.0f, scale * this->m_spacing, scale, this->m_textScale, this->m_rotation);
        if ((this->m_stringFlags & FONTSTRING_SHADOW) != 0) {
            TextBlockSetShadow(this->m_textBlock, ShadowColorWithAlpha(this), this->m_shadowOffset);
        }
        if (-1 < this->m_gradientStart) {
            TextBlockSetGradient(this->m_textBlock, this->m_gradientStart, this->m_gradientLength);
        }
        this->CreateEmbeddedTextures();
        if (this->HasAnimTransform()) {
            ANIMTRANSFORM transform = {};
            transform.m_values[0] = position[0];
            transform.m_values[1] = position[1];
            transform.m_values[2] = position[2];
            this->SetAnimTransform(&transform);
            TextBlockSetStringPos(this->m_textBlock, position);
        }
        CSimpleTop* top = CSimpleTop::s_instance;
        if (top->m_unknown318 == this) {
            top->m_unknown318 = nullptr;
            if (top->m_unknown324 != 0) {
                top->m_unknown324 = 0;
            }
        }
        if ((this->m_textFlags & TEXT_HYPERLINK_MASK) != TEXT_HYPERLINK_MASK) {
            this->m_textFlags |= TEXT_HYPERLINK_MASK;
            if (this->m_parent != nullptr) {
                this->m_parent->RunOnHyperlinkLeave();
            }
        }
    }

    if (this->m_parent != nullptr && (this->m_layoutState & LAYOUT_VISIBLE) != 0 && (this->IsLayoutReady() || hadTextures)) {
        this->m_parent->MarkBatchDirty();
    }
}

// The family's member for the text's script stands in when its file is in; otherwise the file
// is fetched and the string waits for it.
// ref: FUN_140629910
void CSimpleFontString::UpdateFamilyMember(const char* text, bool process, bool remember) {
    if (text != nullptr && process) {
        text = FontStringProcessGrammar(text);
    }
    if (this->m_font && text != nullptr && *text != '\0') {
        this->RestoreFamily();
        int32_t missing = 0;
        int32_t script = GxuFontMissingScript(text, TextBlockGetFontPtr(this->m_font), &missing);
        if (script != 4) {
            for (CSimpleFont* font = this->m_fontObject; font != nullptr; font = font->m_fontObject) {
                CSimpleFont* member = font->m_members[s_scriptMember[script]];
                if (member == nullptr) {
                    continue;
                }
                if (!member->IsFontFileLoaded()) {
                    member->LoadFontFileFor(this);
                } else {
                    this->m_familyFontObject = this->m_fontObject;
                    this->CSimpleFontable::SetFontObject(member);
                    int32_t stillMissing = 0;
                    GxuFontMissingScript(text, TextBlockGetFontPtr(this->m_font), &stillMissing);
                    if (missing <= stillMissing) {
                        this->RestoreFamily();
                    }
                }
                break;
            }
        }
    }
    uint32_t checked = remember && text != nullptr ? TEXT_FAMILY_CHECKED : 0;
    this->m_textFlags = (this->m_textFlags & ~TEXT_FAMILY_CHECKED) | checked;
}

// ref: FUN_140628500
void CSimpleFontString::RestoreFamily() {
    CSimpleFont* family = this->m_familyFontObject;
    if (family == nullptr) {
        return;
    }
    family->m_attributes.m_flags |= CSimpleFontAttributes::RESTORING;
    this->CSimpleFontable::SetFontObject(family);
    family->m_attributes.m_flags &= ~CSimpleFontAttributes::RESTORING;
    this->m_familyFontObject = nullptr;
}

// ref: FUN_1406231c0
void CSimpleFontString::WaitForFontFile() {
    if (this->m_pendingLink.IsLinked()) {
        return;
    }
    s_pendingStrings.m_list.LinkToTail(this);
}

// ref: FUN_140628210
void CSimpleFontString::CreateEmbeddedTextures() {
    GXUEMBEDDEDTEXTURE* textures = nullptr;
    uint32_t count = GxuFontStringGetTextures(this->m_textBlock != nullptr ? this->m_textBlock->m_string : nullptr, &textures);
    for (uint32_t i = 0; i < count; i++) {
        CSimpleEmbeddedTexture* node = s_embeddedCache.m_list.Head();
        if (node != nullptr) {
            node->m_link.Unlink();
        } else {
            node = static_cast<CSimpleEmbeddedTexture*>(SMemAlloc(sizeof(CSimpleEmbeddedTexture), 0x1f, s_sourceFile, 0, 0x93d, 0));
            new (node) CSimpleEmbeddedTexture();
            node->m_texture = new CSimpleTexture(nullptr, 2, 0, true);
            node->m_texture->m_textureFlags |= CSimpleTexture::TEXTURE_4000;
        }
        this->m_embeddedTextures.LinkToTail(node);
        FontStringSetUpEmbeddedTexture(node, this, &textures[i]);
    }
    this->CheckLayoutReady();
}

// ref: FUN_140624b80
void CSimpleFontString::ReleaseEmbeddedTextures() {
    while (CSimpleEmbeddedTexture* node = this->m_embeddedTextures.Head()) {
        node->m_texture->Hide();
        node->m_texture->ClearAllPointsVirtual();
        node->m_link.Unlink();
        s_embeddedCache.m_list.LinkToTail(node);
    }
}

// ---- hit testing ---------------------------------------------------------------------------------

// ref: FUN_140624620
bool CSimpleFontString::GetHyperlinkAt(const float* point, uint32_t* index, CRect* out) {
    if (this->m_parent == nullptr) {
        return false;
    }
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    CRect rect = {};
    if (!this->GetRect(&rect)) {
        return false;
    }
    if (!(rect.minX <= point[0] && point[0] <= rect.maxX && rect.minY <= point[1] && point[1] <= rect.maxY)) {
        return false;
    }
    if (this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return false;
    }
    GXUHYPERLINKINFO* links = nullptr;
    uint32_t count = static_cast<uint32_t>(GxuFontStringGetHyperlinks(this->m_textBlock->m_string, &links));
    if (0xffff < count) {
        count = 0xffff;
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    for (uint32_t i = 0; i < count; i++) {
        const float* r = links[i].m_rect;
        float left = space.m_diagonalWidth * r[1];
        float right = space.m_diagonalWidth * r[3];
        float bottom = r[2] * space.m_diagonalHeight;
        float top = r[0] * space.m_diagonalHeight;
        uint32_t justifyH = this->m_stringFlags & FONTSTRING_JUSTIFY_H;
        if (justifyH != TEXTBLOCK_JUSTIFY_LEFT) {
            if (justifyH == TEXTBLOCK_JUSTIFY_CENTER || justifyH != TEXTBLOCK_JUSTIFY_RIGHT) {
                float shift = (rect.maxX - rect.minX) * 0.5f;
                left += shift;
                right += shift;
            } else {
                right += rect.maxX - rect.minX;
                left += rect.maxX - rect.minX;
            }
        }
        uint32_t justifyV = this->m_stringFlags & FONTSTRING_JUSTIFY_V;
        if (justifyV != FONTSTRING_JUSTIFY_TOP) {
            float span = rect.maxY - rect.minY;
            float scale = this->m_layoutScale;
            float shift;
            if (justifyV == FONTSTRING_JUSTIFY_BOTTOM) {
                shift = span - this->GetStringHeight() * scale;
            } else {
                shift = (span - this->GetStringHeight() * scale) * 0.5f;
            }
            top -= shift;
            bottom -= shift;
        }
        CRect linkRect = { rect.maxY + bottom, rect.minX + left, rect.maxY + top, rect.minX + right };
        CRect screen;
        CRect* transformed = this->m_parent->TransformRect(&screen, &linkRect);
        if (transformed->minX <= point[0] && point[0] <= transformed->maxX && transformed->minY <= point[1]
            && point[1] <= transformed->maxY) {
            if (out != nullptr) {
                out->minY = bottom;
                out->minX = left;
                out->maxY = top;
                out->maxX = right;
            }
            *index = i;
            return true;
        }
    }
    return false;
}

// ref: FUN_1406240a0
bool CSimpleFontString::FindCharacterIndexAtCoordinate(float x, float y, int32_t* index, bool* inside) {
    if (this->m_parent == nullptr) {
        return false;
    }
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    CRect rect = {};
    if (!this->GetRect(&rect) || this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return false;
    }
    float layoutHeight = this->GetLayoutHeight();
    float layoutWidth = this->GetLayoutWidth();
    const char* text = this->GetFittedText(layoutWidth, layoutHeight, nullptr);
    if (text == nullptr) {
        return false;
    }
    float unscale = 1.0f / this->m_layoutScale;
    rect.minY *= unscale;
    rect.minX *= unscale;
    rect.maxY *= unscale;
    rect.maxX *= unscale;
    int32_t offsets[0x41c];
    uint32_t count = this->GetLineStarts(text, rect.maxX - rect.minX, offsets, 0x3ff);
    if (0x3ff < count) {
        count = 0x3ff;
    }
    int32_t length = static_cast<int32_t>(std::strlen(text));
    float span = rect.maxY - rect.minY;
    uint32_t justifyV = this->m_stringFlags & FONTSTRING_JUSTIFY_V;
    offsets[count] = length;
    float down = span - (y - rect.minY);
    if (justifyV != FONTSTRING_JUSTIFY_TOP) {
        if (justifyV == FONTSTRING_JUSTIFY_BOTTOM) {
            down -= span - this->GetStringHeight();
        } else {
            down += (span - this->GetStringHeight()) * -0.5f;
        }
    }
    float lineHeight = this->GetLineHeight();
    uint32_t line = 0;
    while (lineHeight < down && line < count) {
        down -= lineHeight;
        line++;
    }
    uint32_t flags = this->m_stringFlags;
    const char* start = text + offsets[line];
    float left = rect.minX;
    float right = rect.maxX;
    if ((flags & FONTSTRING_JUSTIFY_H) == TEXTBLOCK_JUSTIFY_CENTER) {
        float lineWidth = this->GetTextWidth(start, static_cast<uint32_t>(offsets[line + 1] - offsets[line]));
        flags = this->m_stringFlags;
        float shift = ((rect.maxX - rect.minX) - lineWidth) * 0.5f;
        left = rect.minX + shift;
        right = rect.maxX - shift;
        rect.minX = left;
        rect.maxX = right;
    }
    float across = x - left;
    if (0.0f <= across) {
        if (right < across) {
            across = right;
        }
    } else {
        across = 0.0f;
    }
    uint32_t justifyH = flags & FONTSTRING_JUSTIFY_H;
    uint32_t chars = 0;
    if (justifyH == TEXTBLOCK_JUSTIFY_LEFT || justifyH == TEXTBLOCK_JUSTIFY_CENTER) {
        int32_t lineLength = offsets[line + 1] - offsets[line];
        if (this->m_font) {
            if (lineLength == 0) {
                lineLength = static_cast<int32_t>(std::strlen(start));
            }
            float scale = this->m_layoutScale;
            float extent = 0.0f;
            float height = this->GetFontHeight(true);
            chars = static_cast<uint32_t>(TextBlockGetTextExtent(this->m_font, start, height * scale, scale * across,
                static_cast<uint32_t>(lineLength), &extent, this->m_shadowOffset[0], scale, 0.0f, flags));
        }
        *index = FontStringCharCountToByteOffset(start, static_cast<int32_t>(chars)) + offsets[line];
    } else if (justifyH == TEXTBLOCK_JUSTIFY_RIGHT) {
        int32_t lineLength = offsets[line + 1] - offsets[line];
        if (this->m_font) {
            if (lineLength == 0) {
                lineLength = static_cast<int32_t>(std::strlen(start));
            }
            float scale = this->m_layoutScale;
            float extent = 0.0f;
            float height = this->GetFontHeight(true);
            chars = TextBlockGetMaxCharsWithinWidth(this->m_font, start, height * scale, scale * ((right - left) - across),
                static_cast<uint32_t>(lineLength), &extent, this->m_shadowOffset[0], scale, 0.0f, flags);
        }
        *index = offsets[line + 1] - FontStringCharCountToByteOffset(start, static_cast<int32_t>(chars));
    }
    CRect screen;
    CRect* transformed = this->m_parent->TransformRect(&screen, &rect);
    (void)transformed;
    *inside = rect.minX <= x && x <= rect.maxX && rect.minY <= y && y <= rect.maxY;
    return true;
}

// ref: FUN_140623540
bool CSimpleFontString::CalculateScreenAreaFromCharacterSpan(uint32_t start, uint32_t end, uiBoundsRect** rects,
    uint64_t* count) {
    *rects = nullptr;
    *count = 0;
    if (end < start || this->m_parent == nullptr) {
        return false;
    }
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) != 0) {
        this->Resize(true);
    }
    CRect rect = {};
    if (!this->GetRect(&rect) || this->m_textBlock == nullptr || this->m_textBlock->m_string == nullptr) {
        return false;
    }
    float layoutHeight = this->GetLayoutHeight();
    float layoutWidth = this->GetLayoutWidth();
    const char* text = this->GetFittedText(layoutWidth, layoutHeight, nullptr);
    if (text == nullptr) {
        return false;
    }
    uint32_t length = static_cast<uint32_t>(std::strlen(text));
    if (length < end) {
        return false;
    }
    float unscale = 1.0f / this->m_layoutScale;
    rect.minY *= unscale;
    rect.minX *= unscale;
    rect.maxY *= unscale;
    rect.maxX *= unscale;
    int32_t offsets[0x400];
    uint32_t lines = this->GetLineStarts(text, rect.maxX - rect.minX, offsets, 0x3ff);
    if (0x3ff < lines) {
        lines = 0x3ff;
    }
    offsets[lines] = static_cast<int32_t>(length);
    uint32_t first = 0xffffffff;
    uint32_t last = 0xffffffff;
    for (uint32_t i = 0; i < lines; i++) {
        if (static_cast<uint32_t>(offsets[i]) <= start && start <= static_cast<uint32_t>(offsets[i + 1])) {
            first = i;
        }
        if (static_cast<uint32_t>(offsets[i]) <= end && end <= static_cast<uint32_t>(offsets[i + 1])) {
            last = i;
            break;
        }
    }
    if (first == 0xffffffff || lines <= first || last == 0xffffffff || lines <= last) {
        return false;
    }
    blz::vector<uiBoundsRect, s_boundsTag> bounds;
    for (uint32_t line = first; line <= last; line++) {
        uint32_t justifyH = this->m_stringFlags & FONTSTRING_JUSTIFY_H;
        float left = 0.0f;
        float width = 0.0f;
        uint32_t lineStart = static_cast<uint32_t>(offsets[line]);
        uint32_t lineEnd = static_cast<uint32_t>(offsets[line + 1]);
        auto partial = [&]() {
            if (start == lineStart || line != first) {
                float w = 0.0f;
                if (this->m_font) {
                    float height = this->GetFontHeight(true);
                    TextBlockGetWrappedWidth(this->m_font, text + lineStart, 0, height * this->m_layoutScale, &w,
                        this->m_shadowOffset[0], this->m_layoutScale, 0.0f, this->m_stringFlags);
                    w /= this->m_layoutScale;
                }
                return w;
            }
            return this->GetTextWidth(text + lineStart, start - lineStart);
        };
        if (justifyH == TEXTBLOCK_JUSTIFY_LEFT) {
            width = partial();
        } else if (justifyH == TEXTBLOCK_JUSTIFY_CENTER) {
            float lineWidth = this->GetTextWidth(text + lineStart, lineEnd - lineStart);
            left = (this->GetWidth() - lineWidth) * 0.5f;
            width = partial();
        } else if (justifyH == TEXTBLOCK_JUSTIFY_RIGHT) {
            left = this->GetWidth();
            if (line == first) {
                if (lineEnd == start || (lineEnd - start == 1 && text[start] == '\n')) {
                    left -= this->GetTextWidth(" ", 1);
                } else {
                    left -= this->GetTextWidth(text + start, lineEnd - start);
                }
            } else {
                left -= this->GetTextWidth(text + lineStart, lineEnd - lineStart);
            }
        }
        float lineHeight = this->GetLineHeight();
        uint32_t justifyV = this->m_stringFlags & FONTSTRING_JUSTIFY_V;
        float bottom = static_cast<float>((lines - line) - 1) * lineHeight;
        if (justifyV != FONTSTRING_JUSTIFY_TOP) {
            float span = rect.maxY - rect.minY;
            if (justifyV == FONTSTRING_JUSTIFY_BOTTOM) {
                bottom -= span - this->GetStringHeight();
            } else {
                bottom += (span - this->GetStringHeight()) * 0.5f;
            }
        }
        float spanWidth = this->GetTextWidth(nullptr, 0);
        if ((this->m_stringFlags & FONTSTRING_INDENTED_WORD_WRAP) != 0 && line != 0) {
            left += GxuFontGetIndent() * g_coordinateSpace.m_diagonalWidth;
        }
        if (bounds.size() < 0x400) {
            uiBoundsRect bound = { left + width, bottom, (spanWidth + left) - left, (bottom + lineHeight) - bottom };
            bounds.push_back(bound);
        }
    }
    uint64_t total = bounds.size();
    auto copy = static_cast<uiBoundsRect*>(SMemAlloc(total * sizeof(uiBoundsRect), 0xb, s_boundsTag, 0, -6, 0));
    for (uint64_t i = 0; i < total; i++) {
        copy[i] = bounds.begin()[i];
    }
    *rects = copy;
    *count = total;
    return true;
}

// ---- free functions ------------------------------------------------------------------------------

// ref: FUN_140620970
void RenderBatchAddFontString(CRenderBatch* batch, CSimpleFontString* string) {
    CRect rect = {};
    if (!string->GetRect(&rect)) {
        return;
    }
    bool offscreen = rect.maxY < 0.0f || 1.0f < rect.minY || rect.maxX < 0.0f || 1.0f < rect.minX;
    if (offscreen && !CSimpleFontString::s_drawOffscreen) {
        return;
    }
    if (string->m_textBlock == nullptr || string->m_textBlock->m_string == nullptr) {
        return;
    }
    uint64_t key = string->GetOwnDrawOrderKey();
    GxuStringBatch* strings = batch->GetStringBatch(string->m_parent, key, key);
    GxuStringBatchAddString(strings, string->m_textBlock->m_string);
}

// ref: FUN_140624960
int32_t FontStringCharCountToByteOffset(const char* text, int32_t count) {
    int32_t offset = 0;
    if (count < 1 || text == nullptr) {
        return 0;
    }
    while (true) {
        if (*text == '\0' || count-- == 0) {
            return offset;
        }
        int32_t code;
        int32_t advance = 0;
        uint32_t ignored = 0;
        do {
            code = GxuDetermineControlCode(text, &advance, nullptr, 0, &ignored);
            offset += advance;
            text += advance;
        } while (code == 1 || code == 5 || code == 7 || code == 9 || code == 13);
        if (*text == '\0') {
            return offset;
        }
        while (true) {
            uint32_t next = static_cast<uint32_t>(GxuDetermineControlCode(text, &advance, nullptr, 0, &ignored));
            if (0xe < next || ((0x5544u >> (next & 0x1f)) & 1) == 0) {
                break;
            }
            text += advance;
            offset += advance;
            if (*text == '\0') {
                break;
            }
        }
    }
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_141cb2d80 = (void*)&FontStringExpandCodes;
extern "C" void* const gen_p_142e97be0 = (void*)&s_fontStringHeap;
// ---- end of generated globals ----
