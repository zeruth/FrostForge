#ifndef UI_C_SIMPLE_FONT_HPP
#define UI_C_SIMPLE_FONT_HPP

// CSimpleFont (Engine/Source/Frame/CSimpleFont.cpp): a named font object FrameXML defines with
// <Font> or <FontFamily> and that font strings (and other fonts) take their settings from
// ("Font", 0xc0; vtable 0x1426cb360 with the CSimpleFontable vtable 0x1426cb3d8 at +0x10).
// The settings live in a CSimpleFontAttributes; every fontable using the font is linked into
// its list and told when the settings change. A font family keeps one member font per
// alphabet, and its own settings are those of the client's alphabet.

#include <cstdint>

#include "framescript/FrameScriptObject.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "storm/list/TSLink.hpp"

struct XMLOBJECT;
class CStatus;
class CSimpleFont;
class CSimpleFontString;
struct lua_State;

// A font's settings, and which of them it has (0x30).
struct CSimpleFontAttributes {
    // The low bits say what changed (2 font, 4 justify and wrap flags, 8 colour, 0x10 shadow,
    // 0x20 spacing); the high ones what is set (0x40 font, 0x80 flags, 0x100 colour, 0x200
    // shadow, 0x400 spacing). 0x800 marks a family member being put back.
    enum : uint16_t {
        CHANGED_FONT = 0x2,
        CHANGED_FLAGS = 0x4,
        CHANGED_COLOR = 0x8,
        CHANGED_SHADOW = 0x10,
        CHANGED_SPACING = 0x20,
        CHANGED_ALL = 0x3e,
        HAS_FONT = 0x40,
        HAS_FLAGS = 0x80,
        HAS_COLOR = 0x100,
        HAS_SHADOW = 0x200,
        HAS_SPACING = 0x400,
        RESTORING = 0x800,
    };

    uint16_t m_flags = 0;           // 0x00
    uint16_t m_pad02[3];
    const char* m_fontPath = nullptr;   // 0x08 interned (RCString)
    float m_height = 0.0f;          // 0x10
    uint8_t m_fontFlags = 0;        // 0x14 text block font flags
    uint8_t m_pad15[3];
    float m_spacing = 0.0f;         // 0x18
    uint32_t m_stringFlags = 0x212; // 0x1c justification and wrap (font string flags)
    uint32_t m_color = 0xffffffff;  // 0x20
    uint32_t m_shadowColor = 0;     // 0x24
    float m_shadowOffset[2] = {};   // 0x28

    // Copies what `source` has into this, marking what changed (FUN_140622cf0).
    CSimpleFontAttributes* Merge(const CSimpleFontAttributes* source);
    // The same, limited to the settings `mask` lets through (FUN_1406290f0).
    void MergeMasked(CSimpleFontAttributes* target, uint16_t mask) const;
    // Sets the font file, height and flags; false when the file does not exist
    // (FUN_1406286d0).
    bool SetFont(const char* path, float height, const uint8_t* fontFlags);
};
static_assert(sizeof(CSimpleFontAttributes) == 0x30, "CSimpleFontAttributes is 0x30 bytes");

// What takes its settings from a font object (0x30; vtable 0x1426cad30).
class CSimpleFontable {
    public:
    CSimpleFont* m_fontObject = nullptr;        // 0x08
    // A font family set aside while one of its members stands in for it.
    CSimpleFont* m_familyFontObject = nullptr;  // 0x10
    // The settings that still come from the font object (CSimpleFontAttributes's changed bits).
    uint16_t m_fontableFlags = CSimpleFontAttributes::CHANGED_ALL;  // 0x18
    uint16_t m_pad1a[3];
    TSLink<CSimpleFontable> m_fontableLink;     // 0x20 the font object's fontables

    CSimpleFontable() = default;
    // ref: FUN_140622c40
    virtual ~CSimpleFontable();
    // Moves to another font object and takes its settings (FUN_140628820).
    virtual void SetFontObject(CSimpleFont* font);
    // The font object's settings changed.
    virtual void FontObjectUpdated(CSimpleFontAttributes* attributes) = 0;

    // Leaves the font object's list.
    void UnlinkFontObject();
};
static_assert(sizeof(CSimpleFontable) == 0x30, "CSimpleFontable is 0x30 bytes");

class CSimpleFont : public FrameScript_Object, public CSimpleFontable {
    public:
    // The alphabets a family has a member for.
    enum : int32_t {
        ALPHABET_ROMAN = 0,
        ALPHABET_KOREAN = 1,
        ALPHABET_SIMPLIFIED_CHINESE = 2,
        ALPHABET_TRADITIONAL_CHINESE = 3,
        ALPHABET_RUSSIAN = 4,
        NUM_ALPHABETS = 5,
    };

    CSimpleFontAttributes m_attributes;     // 0x40
    const char* m_name = nullptr;           // 0x70
    uint16_t m_unknown78 = 0;               // 0x78
    uint16_t m_pad7a[3];
    // The fontables using it, linked at CSimpleFontable::m_fontableLink.
    TSExplicitList<CSimpleFontable> m_fontables;    // 0x80
    CSimpleFont* m_members[NUM_ALPHABETS] = {};     // 0x98 a family's fonts

    // The fonts alive (0x142e99c20).
    static int64_t s_count;
    // The client's alphabet, the family member fonts take their settings from (0x142e99c24).
    static int32_t s_alphabet;
    // The metatables (DAT_142cf5330, DAT_142cf5334).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_14062a010
    CSimpleFont();
    // ref: FUN_14062a220
    ~CSimpleFont() override;

    // The font of a name, made and registered as a script global when asked (FUN_14062aad0).
    static CSimpleFont* GetFont(const char* name, bool create);
    // Every named font goes (FUN_14062a9b0).
    static void DestroyFonts();
    // Scales every font not made at a pixel size (FUN_14062b7b0).
    static void ScaleFonts(const float* scale);
    // ref: FUN_14062a960
    static void CreateScriptMetaTables();
    // ref: FUN_14062aa30
    static void DestroyScriptMetaTables();
    // This class's type id (DAT_142e99c28).
    static int32_t GetObjectType();

    // FrameScript_Object.
    const char* GetName() override;
    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;

    // Tells every fontable what changed, then clears the changed bits (FUN_14062bb00).
    virtual void UpdateFontables();

    // CSimpleFontable.
    void FontObjectUpdated(CSimpleFontAttributes* attributes) override;

    // Takes another font's settings and members (FUN_14062a320).
    CSimpleFont& operator=(const CSimpleFont& other);
    // ref: FUN_14062b230
    void LoadXML(XMLOBJECT* node, CStatus* status);
    // Whether the font's file is on disk and its face made (FUN_14062b1f0).
    bool IsFontFileLoaded();
    // Starts loading the font's file for a font string waiting on it (FUN_14062a8c0).
    void LoadFontFileFor(CSimpleFontString* string);
    // The font file, height and flags (FUN_14062aab0).
    void GetFontInfo(const char** path, float* height, uint8_t* flags);
};
static_assert(sizeof(CSimpleFont) == 0xc0, "CSimpleFont is 0xc0 bytes");

// A height in FrameXML units (pixels at 768 high) as a UI height, at most 0x1426cb338
// (FUN_14062a930).
float CSimpleFont_ConvertHeight(float height);
void CSimpleFont_FillScriptMethods(lua_State* L);

#endif
