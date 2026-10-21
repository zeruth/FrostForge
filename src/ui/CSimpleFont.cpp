#include "ui/CSimpleFont.hpp"

#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "base/RCString.hpp"
#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "fileservices/AsyncFile.hpp"
#include "framescript/FrameScript.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

bool ClientUI_IsTearingDown();

int64_t CSimpleFont::s_count;
int32_t CSimpleFont::s_alphabet;
int32_t CSimpleFont::s_metaTable = -2;
int32_t CSimpleFont::s_restrictedMetaTable = -2;

// A named font in the table (0x40): the node, the name it is found by, the font.
struct SIMPLEFONT {
    TSLink<SIMPLEFONT> m_linktoslot;    // 0x00
    TSLink<SIMPLEFONT> m_linktofull;    // 0x10
    void* m_table = nullptr;            // 0x20
    uint32_t m_hashval = 0;             // 0x28
    uint32_t m_pad2c = 0;
    char* m_str = nullptr;              // 0x30 HASHKEY_STRI
    CSimpleFont* m_font = nullptr;      // 0x38
};
static_assert(sizeof(SIMPLEFONT) == 0x40, "SIMPLEFONT is 0x40 bytes");

template <>
struct TSHashTableTags<SIMPLEFONT> {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@USIMPLEFONT@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@USIMPLEFONT@@$0?CCCCCCCD@$07@@$07@@";
};

namespace {

const char s_sourceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleFont.cpp";
const char s_fontTableTag[] = ".?AV?$TSHashTable@USIMPLEFONT@@VHASHKEY_STRI@@$07@@";
const char s_fontListTag[] = ".?AV?$TSList@USIMPLEFONT@@V?$TSGetExplicitLink@USIMPLEFONT@@@@$07@@";

constexpr float EPSILON = 2.3841858e-07f;      // 0x14261a954
constexpr float MAX_FONT_HEIGHT = 120.0f;      // 0x1426cb338

float Abs(float value) {
    return value < 0.0f ? -value : value;
}

// The font's sizing allocation and construction (the reference's tagged new at each site).
CSimpleFont* NewFont(int32_t line) {
    void* memory = SMemAlloc(sizeof(CSimpleFont), 0x1f, s_sourceFile, 0, line, 0);
    return new (memory) CSimpleFont();
}

// TSHashTable<SIMPLEFONT, HASHKEY_STRI, 8> (vtable 0x1426cb340; the static at 0x142cf5340).
class SIMPLEFONTTABLE : public TSHashTable<SIMPLEFONT> {
    public:
    // ref: FUN_14062a1a0
    ~SIMPLEFONTTABLE() override = default;

    // ref: FUN_140629df0
    void Delete(SIMPLEFONT* node) override {
        if (node->m_font != nullptr) {
            delete node->m_font;
        }
        // the hash object's destructor (FUN_140613680): the node leaves its table, the key goes
        if (node->m_table != nullptr) {
            static_cast<SIMPLEFONTTABLE*>(node->m_table)->Unlink(node);
        }
        node->m_linktoslot.Unlink();
        node->m_linktofull.Unlink();
        SMemFree(node->m_str, nullptr, 0, 0);
        node->m_str = nullptr;
        SMemFree(node, s_fontTableTag, -2, 0);
    }

    // ref: FUN_140629e30
    SIMPLEFONT* NewNode(TSExplicitList<SIMPLEFONT>* slot, uint32_t extra, uint32_t flags) override {
        void* memory = SMemAlloc(extra + sizeof(SIMPLEFONT), 8, s_fontListTag, 0, -2, flags | 8);
        auto node = new (memory) SIMPLEFONT();
        node->m_font = NewFont(0x1a);
        slot->LinkToTail(node);
        return node;
    }
};

SIMPLEFONTTABLE s_fonts;

// The name messages use: the font's, or its type's.
const char* DisplayName(CSimpleFont* font) {
    const char* name = font->GetName();
    return name != nullptr ? name : font->GetObjectTypeName();
}

}

// ---- CSimpleFontAttributes -------------------------------------------------------------------

// ref: FUN_140622cf0
CSimpleFontAttributes* CSimpleFontAttributes::Merge(const CSimpleFontAttributes* source) {
    source->MergeChanged(this, 0xffff);
    return this;
}

// ref: FUN_1406290f0
void CSimpleFontAttributes::MergeMasked(CSimpleFontAttributes* target, uint16_t mask) const {
    this->MergeChanged(target, mask & this->m_flags);
}

// What `source` has set of the attributes `changed` names goes into `target`. MergeMasked
// passes the source's own changed bits under its mask; Merge (FUN_140622cf0) checks only what
// the source has, so it passes every bit (a font settled by UpdateFontables has none changed,
// and CopyFontObject copied nothing from one until 2026-10-21).
void CSimpleFontAttributes::MergeChanged(CSimpleFontAttributes* target, uint16_t mask) const {
    if ((this->m_flags & HAS_FONT) != 0 && (mask & CHANGED_FONT) != 0) {
        const char* path = this->m_fontPath;
        float height = this->m_height;
        if (path != nullptr) {
            bool same = target->m_fontPath != nullptr && this->m_fontFlags == target->m_fontFlags
                && Abs(height - target->m_height) < EPSILON && _stricmp(path, target->m_fontPath) == 0;
            if (!same && *path != '\0' && 0.0f < height && cas::CASFileExistsByName(path, 1)) {
                target->m_fontPath = reinterpret_cast<RCString*>(&target->m_fontPath)->Intern(path);
                target->m_height = height;
                target->m_fontFlags = this->m_fontFlags;
                target->m_flags |= HAS_FONT | CHANGED_FONT;
            }
        }
    }
    if ((this->m_flags & HAS_FLAGS) != 0 && (mask & CHANGED_FLAGS) != 0
        && (this->m_stringFlags != target->m_stringFlags || (target->m_flags & HAS_FLAGS) == 0)) {
        target->m_stringFlags = this->m_stringFlags;
        target->m_flags |= HAS_FLAGS | CHANGED_FLAGS;
    }
    if ((this->m_flags & HAS_COLOR) != 0 && (mask & CHANGED_COLOR) != 0
        && (this->m_color != target->m_color || (target->m_flags & HAS_COLOR) == 0)) {
        target->m_color = this->m_color;
        target->m_flags |= HAS_COLOR | CHANGED_COLOR;
    }
    if ((this->m_flags & HAS_SHADOW) != 0 && (mask & CHANGED_SHADOW) != 0
        && (this->m_shadowColor != target->m_shadowColor || target->m_shadowOffset[0] != this->m_shadowOffset[0]
            || target->m_shadowOffset[1] != this->m_shadowOffset[1] || (target->m_flags & HAS_SHADOW) == 0)) {
        target->m_shadowColor = this->m_shadowColor;
        target->m_shadowOffset[0] = this->m_shadowOffset[0];
        target->m_shadowOffset[1] = this->m_shadowOffset[1];
        target->m_flags |= HAS_SHADOW | CHANGED_SHADOW;
    }
    if ((this->m_flags & HAS_SPACING) != 0 && (mask & CHANGED_SPACING) != 0
        && (EPSILON <= Abs(this->m_spacing - target->m_spacing) || (target->m_flags & HAS_SPACING) == 0)) {
        target->m_spacing = this->m_spacing;
        target->m_flags |= HAS_SPACING | CHANGED_SPACING;
    }
}

// ref: FUN_1406286d0
bool CSimpleFontAttributes::SetFont(const char* path, float height, const uint8_t* fontFlags) {
    if (path == nullptr) {
        return false;
    }
    if (this->m_fontPath != nullptr && *fontFlags == this->m_fontFlags && Abs(height - this->m_height) < EPSILON
        && _stricmp(path, this->m_fontPath) == 0) {
        return true;
    }
    if (*path == '\0' || !(0.0f < height) || !cas::CASFileExistsByName(path, 1)) {
        return false;
    }
    this->m_fontPath = reinterpret_cast<RCString*>(&this->m_fontPath)->Intern(path);
    this->m_height = height;
    this->m_fontFlags = *fontFlags;
    this->m_flags |= HAS_FONT | CHANGED_FONT;
    return true;
}

// ---- CSimpleFontable -------------------------------------------------------------------------

CSimpleFontable::~CSimpleFontable() {
    this->UnlinkFontObject();
    this->m_fontableLink.Unlink();
}

void CSimpleFontable::UnlinkFontObject() {
    if (this->m_fontObject != nullptr) {
        this->m_fontObject->m_fontables.Link(this)->Unlink();
        this->m_fontObject = nullptr;
    }
}

// ref: FUN_140628820
void CSimpleFontable::SetFontObject(CSimpleFont* font) {
    if (this->m_fontObject == font) {
        return;
    }
    if (this->m_fontObject != nullptr) {
        this->m_fontObject->m_fontables.Link(this)->Unlink();
    }
    this->m_fontObject = font;
    if (font == nullptr) {
        return;
    }
    font->m_fontables.LinkToTail(this);
    font->m_attributes.m_flags |= CSimpleFontAttributes::CHANGED_ALL;
    this->FontObjectUpdated(&font->m_attributes);
    font->m_attributes.m_flags &= ~CSimpleFontAttributes::CHANGED_ALL;
}

// ---- CSimpleFont -----------------------------------------------------------------------------

// ref: FUN_14062a010
CSimpleFont::CSimpleFont() {
    this->m_attributes.m_stringFlags |= 0x212;
    this->m_fontables.InitEmpty(0x20);
    s_count++;
}

CSimpleFont::~CSimpleFont() {
    if (!ClientUI_IsTearingDown()) {
        this->Unregister(this->m_name);
    }
    while (CSimpleFontable* fontable = this->m_fontables.Head()) {
        fontable->SetFontObject(nullptr);
    }
    for (CSimpleFont*& member : this->m_members) {
        if (member != nullptr) {
            delete member;
            member = nullptr;
        }
    }
    s_count--;
    this->m_fontables.UnlinkAll();
    this->m_fontables.m_terminator.Unlink();
}

// ref: FUN_14062aad0
CSimpleFont* CSimpleFont::GetFont(const char* name, bool create) {
    SIMPLEFONT* node = s_fonts.Ptr(name);
    if (node == nullptr) {
        if (!create) {
            return nullptr;
        }
        uint32_t hash = SStrHashHT(name);
        node = s_fonts.New(hash, 0, 0);
        node->m_hashval = hash;
        char* key = node->m_str;
        if (key != name) {
            SMemFree(key, nullptr, 0, 0);
            node->m_str = nullptr;
            key = SStrDupA(name, nullptr, 0);
            node->m_str = key;
        }
        CSimpleFont* font = node->m_font;
        font->Unregister(font->m_name);
        font->m_name = key;
        font->Register(key);
    }
    return node->m_font;
}

// ref: FUN_14062a9b0
void CSimpleFont::DestroyFonts() {
    s_fonts.Clear(false);
}

// ref: FUN_14062b7b0
void CSimpleFont::ScaleFonts(const float* scale) {
    auto rescale = [scale](CSimpleFont* font) {
        if ((font->m_attributes.m_fontFlags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
            font->m_attributes.m_flags |= CSimpleFontAttributes::CHANGED_FONT;
            font->m_attributes.m_height *= *scale;
        }
        font->UpdateFontables();
    };
    for (SIMPLEFONT* node = s_fonts.m_fulllist.Head(); node != nullptr; node = s_fonts.m_fulllist.Link(node)->Next()) {
        CSimpleFont* font = node->m_font;
        if (font->m_fontObject == nullptr && (font->m_attributes.m_fontFlags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
            font->m_attributes.m_flags |= CSimpleFontAttributes::CHANGED_FONT;
            font->m_attributes.m_height *= *scale;
        }
        font->UpdateFontables();
        for (CSimpleFont* member : font->m_members) {
            if (member != nullptr) {
                rescale(member);
            }
        }
    }
}

// ref: FUN_14062a960
void CSimpleFont::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFont_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFont_FillScriptMethods, true);
}

// ref: FUN_14062aa30
void CSimpleFont::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// The reference's type id and its magic-static guard (CSimpleFont), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleFontType;  // DAT_142e99c28
int32_t g_cSimpleFontTypeGuard;  // DAT_142e99c2c

int32_t CSimpleFont::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleFontType, &g_cSimpleFontTypeGuard);
}

// ref: FUN_14062abb0
const char* CSimpleFont::GetName() {
    return this->m_name;
}

// ref: FUN_14062abd0
int32_t CSimpleFont::GetScriptMetaTable() {
    if ((this->m_flags & 1) == 0 && (this->m_flags & 4) == 0) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_14062b1b0
bool CSimpleFont::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Font") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_14062b110
bool CSimpleFont::IsObjectType(int32_t type) {
    return type == CSimpleFont::GetObjectType() || this->FrameScript_Object::IsObjectType(type);
}

// ref: FUN_14062abc0
const char* CSimpleFont::GetObjectTypeName() {
    return "Font";
}

uint32_t CSimpleFont::GetObjectSize() {
    return 0xc0;
}

// ref: FUN_14062bb00
void CSimpleFont::UpdateFontables() {
    if (this->m_attributes.m_flags == 0) {
        return;
    }
    for (CSimpleFontable* fontable = this->m_fontables.Head(); fontable != nullptr;) {
        CSimpleFontable* next = this->m_fontables.Link(fontable)->Next();
        fontable->FontObjectUpdated(&this->m_attributes);
        fontable = next;
    }
    this->m_attributes.m_flags &= ~CSimpleFontAttributes::CHANGED_ALL;
}

// ref: FUN_14062aa80
void CSimpleFont::FontObjectUpdated(CSimpleFontAttributes* attributes) {
    attributes->MergeMasked(&this->m_attributes, this->m_fontableFlags);
    this->UpdateFontables();
}

// ref: FUN_14062a320
CSimpleFont& CSimpleFont::operator=(const CSimpleFont& other) {
    if (this == &other) {
        return *this;
    }
    this->m_fontableFlags = other.m_fontableFlags;
    this->m_unknown78 = other.m_unknown78;
    this->m_attributes.Merge(&other.m_attributes);

    CSimpleFont* inherited = other.m_fontObject;
    for (CSimpleFont* font = inherited; font != nullptr; font = font->m_fontObject) {
        if (font == this) {
            inherited = nullptr;
            break;
        }
    }

    for (int32_t i = 0; i < NUM_ALPHABETS; i++) {
        if (this->m_members[i] != nullptr) {
            delete this->m_members[i];
        }
        if (other.m_members[i] == nullptr) {
            this->m_members[i] = nullptr;
        } else {
            this->m_members[i] = NewFont(0x15c);
            *this->m_members[i] = *other.m_members[i];
        }
    }

    if (inherited != this->m_fontObject) {
        if (this->m_fontObject != nullptr) {
            this->m_fontObject->m_fontables.Link(this)->Unlink();
        }
        this->m_fontObject = inherited;
        if (inherited != nullptr) {
            inherited->m_fontables.LinkToTail(this);
        }
    }
    this->UpdateFontables();
    return *this;
}

// The font it inherits, then its own settings: the file and height, spacing, justification,
// wrapping, colour and shadow.
// ref: FUN_14062b230
void CSimpleFont::LoadXML(XMLOBJECT* node, CStatus* status) {
    auto xml = static_cast<XMLFONT*>(node);
    if ((node->m_attributes & 0x100) != 0) {
        SIMPLEFONT* inherited = s_fonts.Ptr(node->m_inherits.m_text);
        if (inherited == nullptr) {
            status->Add(1, "Couldn't find inherited font: %s", node->m_inherits.m_text);
        } else {
            this->CSimpleFontable::SetFontObject(inherited->m_font);
        }
    }
    if ((node->m_flags & 0x100000) == 0) {
        return;
    }

    CSimpleFontAttributes& attributes = this->m_attributes;
    if ((xml->m_fontFlags & 0x80) != 0) {
        const char* path = xml->m_font.m_text;
        SIMPLEFONT* named = s_fonts.Ptr(path);
        if (named != nullptr && named->m_font != nullptr) {
            this->CSimpleFontable::SetFontObject(named->m_font);
        } else {
            uint8_t flags = 0;
            if ((xml->m_fontFlags & 0x100) != 0) {
                const char* outline = xml->m_outline.m_text;
                if (_stricmp(outline, "NORMAL") == 0) {
                    flags |= 1;
                } else if (_stricmp(outline, "THICK") == 0) {
                    flags |= 5;
                }
            }
            uint32_t fontFlags = xml->m_fontFlags;
            if ((fontFlags & 0x600) == 0x600) {
                flags |= 2;
            }
            if ((fontFlags & 0x3000) == 0x3000) {
                flags |= 8;
            }
            if ((fontFlags & 0xc000) == 0xc000) {
                flags |= 0x10;
            }
            float height = 0.0f;
            if ((fontFlags & 0x800) == 0) {
                XMLOBJECT* child = XMLObjectFindChild(node, 100);
                if (child != nullptr) {
                    XMLValueGet(static_cast<XMLVALUE*>(child), &height, status);
                    if (MAX_FONT_HEIGHT <= height) {
                        height = MAX_FONT_HEIGHT;
                    }
                }
            } else {
                height = xml->m_height;
                if (MAX_FONT_HEIGHT <= height) {
                    height = MAX_FONT_HEIGHT;
                }
                if ((flags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
                    height = CSimpleFont_ConvertHeight(height);
                }
            }
            if (height <= 0.0f) {
                status->Add(1, "Font %s: Missing font height in %s element", DisplayName(this), node->m_tag.m_text);
                return;
            }
            if (!attributes.SetFont(path, height, &flags)) {
                status->Add(1, "Font %s: Unable to load font file %s", DisplayName(this), path);
            } else {
                this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FONT;
            }
        }
    }

    if ((xml->m_fontFlags & 0x20) != 0) {
        const COORDINATESPACE& space = g_coordinateSpace;
        float spacing = (xml->m_spacing / (space.m_aspectCompensation * 1024.0f)) * space.m_diagonalWidth;
        if (EPSILON <= Abs(spacing - attributes.m_spacing) || (attributes.m_flags & CSimpleFontAttributes::HAS_SPACING) == 0) {
            attributes.m_spacing = spacing;
            attributes.m_flags |= CSimpleFontAttributes::HAS_SPACING | CSimpleFontAttributes::CHANGED_SPACING;
        }
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SPACING;
    }

    auto setFlags = [&attributes](uint32_t value) {
        if (value != attributes.m_stringFlags || (attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS) == 0) {
            attributes.m_stringFlags = value;
            attributes.m_flags |= CSimpleFontAttributes::HAS_FLAGS | CSimpleFontAttributes::CHANGED_FLAGS;
        }
    };
    uint32_t justify = 0;
    if ((xml->m_fontFlags & 1) != 0 && StringToJustify(xml->m_justifyV.m_text, &justify)) {
        setFlags((attributes.m_stringFlags & ~0x38u) | (justify & 0x38));
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    }
    if ((xml->m_fontFlags & 2) != 0 && StringToJustify(xml->m_justifyH.m_text, &justify)) {
        setFlags((attributes.m_stringFlags & ~7u) | (justify & 7));
        this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    }
    if ((xml->m_fontFlags & 4) != 0) {
        uint32_t value = attributes.m_stringFlags;
        value = ((xml->m_fontFlags >> 6) & 1) != 0 ? (value | 0x1000) : (value & ~0x1000u);
        setFlags(value);
    }
    if ((xml->m_fontFlags & 8) != 0) {
        uint32_t value = attributes.m_stringFlags;
        value = ((xml->m_fontFlags >> 4) & 1) != 0 ? (value | 0x20000) : (value & ~0x20000u);
        setFlags(value);
    }

    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_COLOR) {
            uint32_t color = XmlColorPack(static_cast<XMLCOLOR*>(child));
            if (color != attributes.m_color || (attributes.m_flags & CSimpleFontAttributes::HAS_COLOR) == 0) {
                attributes.m_color = color;
                attributes.m_flags |= CSimpleFontAttributes::HAS_COLOR | CSimpleFontAttributes::CHANGED_COLOR;
            }
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
            if (color != attributes.m_shadowColor || offset[0] != attributes.m_shadowOffset[0]
                || offset[1] != attributes.m_shadowOffset[1] || (attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW) == 0) {
                attributes.m_shadowColor = color;
                attributes.m_shadowOffset[0] = offset[0];
                attributes.m_shadowOffset[1] = offset[1];
                attributes.m_flags |= CSimpleFontAttributes::HAS_SHADOW | CSimpleFontAttributes::CHANGED_SHADOW;
            }
            this->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
        }
    }
    this->UpdateFontables();
}

// ref: FUN_14062b1f0
bool CSimpleFont::IsFontFileLoaded() {
    return cas::CASGetResidencyByName(this->m_attributes.m_fontPath, 0, 0, 0) == 2
        && GxuFontFaceExistsByName(this->m_attributes.m_fontPath);
}

// ref: FUN_14062a8c0
void CSimpleFont::LoadFontFileFor(CSimpleFontString* string) {
    string->WaitForFontFile();
    const char* path = this->m_attributes.m_fontPath;
    AsyncFileReadByName(path, 0, const_cast<char*>(path), nullptr, CSimpleFontString::AllocFontFileBuffer, nullptr,
        CSimpleFontString::OnFontFileLoaded, CSimpleFontString::OnFontFileError, CSimpleFontString::OnFontFileCanceled);
}

// ref: FUN_14062aab0
void CSimpleFont::GetFontInfo(const char** path, float* height, uint8_t* flags) {
    *path = this->m_attributes.m_fontPath;
    *height = this->m_attributes.m_height;
    *flags = this->m_attributes.m_fontFlags;
}

// ref: FUN_14062a930
float CSimpleFont_ConvertHeight(float height) {
    if (MAX_FONT_HEIGHT <= height) {
        height = MAX_FONT_HEIGHT;
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    return (height / (space.m_aspectCompensation * 1024.0f)) * space.m_diagonalWidth;
}
