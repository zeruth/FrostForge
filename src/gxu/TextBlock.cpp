#include "gxu/TextBlock.hpp"

#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "cas/CAS.hpp"
#include "base/Handle.hpp"
#include "gxu/GxuFont.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

template <>
struct TSHashTableTags<FONTHASHOBJ> {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@UFONTHASHOBJ@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@UFONTHASHOBJ@@$0?CCCCCCCD@$07@@$07@@";
};

namespace {

const char s_fontTableTag[] = ".?AV?$TSHashTable@UFONTHASHOBJ@@UFontHashKey@@$07@@";
const char s_fontListTag[] = ".?AV?$TSList@UFONTHASHOBJ@@V?$TSGetExplicitLink@UFONTHASHOBJ@@@@$07@@";
const char s_sharedPtrFile[] = "D:\\BuildServer\\A\\work\\package_cache\\blz\\3.11.13\\include\\blz\\mem_shared_ptr.h";

// The largest height a font is made at, in Gxu units (DAT_142618d64).
constexpr float s_maxFontHeight = 1.0f;

// TSHashTable<FONTHASHOBJ, FontHashKey, 8> (vtable 0x142726538; the static at 0x142cfbe90).
class FONTTABLE : public TSHashTable<FONTHASHOBJ> {
    public:
    // ref: FUN_141284490
    ~FONTTABLE() override = default;

    // ref: FUN_141283d90
    void Delete(FONTHASHOBJ* node) override;

    // ref: FUN_141283df0
    FONTHASHOBJ* NewNode(TSExplicitList<FONTHASHOBJ>* slot, uint32_t extra, uint32_t flags) override {
        void* memory = SMemAlloc(extra + sizeof(FONTHASHOBJ), 8, s_fontListTag, 0, -2, flags | 8);
        auto node = new (memory) FONTHASHOBJ();
        slot->LinkToTail(node);
        return node;
    }
};

FONTTABLE s_fontTable;

// Takes a node out of the table it is in (the TSHashObject destructor, FUN_140688840).
void UnlinkFont(FONTHASHOBJ* node) {
    if (node->m_table != nullptr) {
        static_cast<FONTTABLE*>(node->m_table)->Unlink(node);
    }
    node->m_linktoslot.Unlink();
    node->m_linktofull.Unlink();
}

// ref: FUN_141283d90
void FONTTABLE::Delete(FONTHASHOBJ* node) {
    if (node->m_font != nullptr) {
        GxuFontDestroyFont(&node->m_font);
    }
    node->m_self.reset();
    UnlinkFont(node);
    node->~FONTHASHOBJ();
    SMemFree(node, s_fontTableTag, -2, 0);
}

// The control block of a cached font (vtable 0x142726598): the last strong reference
// destroys the font and takes the node out of the cache.
class FontSharedCount : public blz::shared_count {
    public:
    FONTHASHOBJ* m_object;      // 0x10
    uint64_t m_deleter = 0;     // 0x18

    explicit FontSharedCount(FONTHASHOBJ* object) : m_object(object) {}

    // ref: FUN_141285aa0
    void* get_deleter() override {
        return &this->m_deleter;
    }

    // ref: FUN_141285a40
    void dispose() override {
        FONTHASHOBJ* node = this->m_object;
        if (node == nullptr) {
            return;
        }
        if (node->m_font != nullptr) {
            GxuFontDestroyFont(&node->m_font);
        }
        node->m_self.reset();
        UnlinkFont(node);
        node->~FONTHASHOBJ();
        operator delete(node);
    }

    // ref: FUN_1406a2020
    void destroy() override {
        delete this;
    }
};
static_assert(sizeof(FontSharedCount) == 0x20, "the font's control block is 0x20 bytes");

// The cache's hash: FNV-1a over the file and the flags.
uint32_t FontHash(const FontHashKey& key) {
    const auto bytes = reinterpret_cast<const uint8_t*>(&key);
    uint32_t hash = 0x811c9dc5;
    for (int32_t i = 0; i < 5; i++) {
        hash = (hash ^ bytes[i]) * 0x1000193;
    }
    return hash;
}

// ref: FUN_1412845e0
HTEXTFONT FindFont(const FontHashKey& key) {
    uint32_t hash = FontHash(key);
    FONTHASHOBJ* node = s_fontTable.Find(hash, [&key](FONTHASHOBJ* candidate) {
        return candidate->m_key.m_fileDataId == key.m_fileDataId && candidate->m_key.m_flags == key.m_flags
            && GxuFontGetPixelSize(candidate->m_key.m_font) == key.m_pixelSize;
    });
    if (node == nullptr) {
        return HTEXTFONT();
    }
    return node->m_self.lock();
}

// ref: FUN_141284d10
HTEXTFONT InsertFont(const FontHashKey& key, CGxuFont* font) {
    uint32_t hash = FontHash(key);
    FONTHASHOBJ* node = s_fontTable.New(hash, 0, 0);
    node->m_hashval = hash;
    node->m_key = key;
    node->m_font = font;

    uint32_t tagHash = 0x811c9dc5;
    for (const char* c = s_sharedPtrFile; *c != '\0'; c++) {
        tagHash = (tagHash ^ static_cast<uint8_t>(*c)) * 0x1000193;
    }
    auto count = new (0xb, s_sharedPtrFile, tagHash, 0x261) FontSharedCount(node);
    HTEXTFONT result(node, count);
    node->m_self = result;
    return result;
}

// Text block font flags to Gxu font flags.
uint32_t ConvertFontFlags(uint8_t flags) {
    uint32_t gxu = flags & TEXTBLOCK_FONT_OUTLINE;
    if ((flags & TEXTBLOCK_FONT_THICK) != 0) {
        gxu |= 8;
    }
    if ((flags & TEXTBLOCK_FONT_MONOCHROME) != 0) {
        gxu |= 2;
    }
    if ((flags & TEXTBLOCK_FONT_8) != 0) {
        gxu |= 4;
    }
    if ((flags & TEXTBLOCK_FONT_FIXED_SIZE) != 0) {
        gxu |= 0x10;
    }
    return gxu;
}

float GenerateHeight(uint8_t flags, float height) {
    if ((flags & TEXTBLOCK_FONT_FIXED_SIZE) == 0) {
        height = TextBlockDDCToNDCHeight(height);
        if (s_maxFontHeight <= height) {
            height = s_maxFontHeight;
        }
    }
    return height;
}

}

// ---- units -----------------------------------------------------------------------------------

// ref: FUN_141284590
void TextBlockDDCToNDC(float x, float y, float* outX, float* outY) {
    if (outX != nullptr) {
        *outX = x / g_coordinateSpace.m_diagonalWidth;
    }
    if (outY != nullptr) {
        *outY = y / g_coordinateSpace.m_diagonalHeight;
    }
}

// ref: FUN_1412845c0
float TextBlockDDCToNDCHeight(float height) {
    return height / g_coordinateSpace.m_diagonalHeight;
}

// ref: FUN_1412845d0
float TextBlockDDCToNDCWidth(float width) {
    return width / g_coordinateSpace.m_diagonalWidth;
}

// ref: FUN_141284cd0
void TextBlockNDCToDDC(float x, float y, float* outX, float* outY) {
    if (outX != nullptr) {
        *outX = x * g_coordinateSpace.m_diagonalWidth;
    }
    if (outY != nullptr) {
        *outY = y * g_coordinateSpace.m_diagonalHeight;
    }
}

// ref: FUN_141284d00
float TextBlockNDCToDDCHeight(float height) {
    return height * g_coordinateSpace.m_diagonalHeight;
}

// ref: FUN_141284710
uint32_t TextBlockConvertFlags(uint32_t flags) {
    uint32_t gxu = (flags >> 8) & 1;
    if ((flags >> 9) & 1) {
        gxu |= GXU_STRING_ONE_TO_ONE;
    }
    if ((flags >> 10) & 1) {
        gxu |= GXU_STRING_NO_COLOR_CODES;
    }
    if ((flags >> 11) & 1) {
        gxu |= GXU_STRING_MONOSPACE;
    }
    if ((flags & 0x40) != 0) {
        gxu |= GXU_STRING_SINGLE_LINE;
    }
    if ((flags >> 12) & 1) {
        gxu |= GXU_STRING_BREAK_ANYWHERE;
    }
    if ((flags >> 13) & 1) {
        gxu |= 0x100;
    }
    if ((flags >> 14) & 1) {
        gxu |= 0x200;
    }
    if ((flags >> 15) & 1) {
        gxu |= 0x400;
    }
    if ((flags & 0x80) != 0) {
        gxu |= 0x800;
    }
    if ((flags >> 16) & 1) {
        gxu |= 0x1000;
    }
    if ((flags >> 17) & 1) {
        gxu |= GXU_STRING_INDENT;
    }
    return gxu;
}

// ---- fonts -----------------------------------------------------------------------------------

// ref: FUN_141283fd0
HTEXTFONT TextBlockGenerateFont(int32_t fileDataId, uint8_t flags, float height) {
    FontHashKey key = {};
    key.m_flags = flags & 0xdf;
    key.m_fileDataId = fileDataId;
    height = GenerateHeight(flags, height);
    uint32_t gxuFlags = ConvertFontFlags(flags);

    GXUFONTPIXELSIZE size = GxuFontGetFacePixelSize(static_cast<uint32_t>(fileDataId), height, gxuFlags);
    if (size.m_result == 1) {
        key.m_pixelSize = size.m_pixelSize;
        HTEXTFONT found = FindFont(key);
        if (found) {
            if ((flags & TEXTBLOCK_FONT_20) != 0) {
                GxuFontSetFlag40(found->m_font);
            }
            return found;
        }
    }

    CGxuFont* font = nullptr;
    if (!GxuFontCreateFont(fileDataId, height, &font, gxuFlags)) {
        return HTEXTFONT();
    }
    key.m_font = font;
    return InsertFont(key, font);
}

// ref: FUN_1412841c0
HTEXTFONT TextBlockGenerateFont(const char* path, uint8_t flags, float height) {
    FontHashKey key = {};
    key.m_fileDataId = cas::CASFileGetIdByName(path, 1);
    key.m_flags = flags & 0xdf;
    height = GenerateHeight(flags, height);
    uint32_t gxuFlags = ConvertFontFlags(flags);

    GXUFONTPIXELSIZE size = GxuFontGetFacePixelSizeByName(path, height, gxuFlags);
    if (size.m_result == 1) {
        key.m_pixelSize = size.m_pixelSize;
        HTEXTFONT found = FindFont(key);
        if (found) {
            if ((flags & TEXTBLOCK_FONT_20) != 0) {
                GxuFontSetFlag40(found->m_font);
            }
            return found;
        }
    }

    CGxuFont* font = nullptr;
    if (!GxuFontCreateFontByName(path, height, &font, gxuFlags)) {
        return HTEXTFONT();
    }
    key.m_font = font;
    return InsertFont(key, font);
}

// ref: FUN_1412858c0
void TextBlockDestroyFonts() {
    s_fontTable.Clear(false);
}

// ref: FUN_141285280
int32_t TextBlockGetFontFileDataId(const HTEXTFONT& font) {
    return GxuFontGetFileDataId(font->m_font);
}

// ref: FUN_141285290
uint8_t TextBlockGetFontFlags(const HTEXTFONT& font) {
    uint32_t gxu = GxuFontGetFlags(font->m_font);
    uint8_t flags = 0;
    if ((gxu & 1) != 0) {
        flags = TEXTBLOCK_FONT_OUTLINE;
    }
    if ((gxu & 8) != 0) {
        flags |= TEXTBLOCK_FONT_THICK;
    }
    if ((gxu & 2) != 0) {
        flags |= TEXTBLOCK_FONT_MONOCHROME;
    }
    if ((gxu & 4) != 0) {
        flags |= TEXTBLOCK_FONT_8;
    }
    if ((gxu & 0x10) != 0) {
        flags |= TEXTBLOCK_FONT_FIXED_SIZE;
    }
    if ((gxu & 0x40) != 0) {
        flags |= TEXTBLOCK_FONT_20;
    }
    return flags;
}

// ref: FUN_1412852f0
const char* TextBlockGetFontName(const HTEXTFONT& font) {
    return GxuFontGetFontName(font->m_font);
}

// ref: FUN_141285300
CGxuFont* TextBlockGetFontPtr(const HTEXTFONT& font) {
    return font->m_font;
}

// ref: FUN_141285310
float TextBlockGetFontHeight(const HTEXTFONT& font, float height, uint32_t flags) {
    float ndc = TextBlockDDCToNDCHeight(height);
    float effective = GxuFontGetEffectiveHeight(font->m_font, ndc, TextBlockConvertFlags(flags));
    return TextBlockNDCToDDCHeight(effective);
}

// ---- measuring -------------------------------------------------------------------------------

// ref: FUN_141285360
int32_t TextBlockGetTextExtent(const HTEXTFONT& font, const char* text, float height, float width, uint32_t length,
    float* extent, float a7, float scale, float a9, uint32_t flags) {
    float ndcA9 = TextBlockDDCToNDCWidth(a9);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    int32_t result = GxuFontGetTextExtent(font->m_font, text, ndcHeight, ndcWidth, length, extent, a7, scale, ndcA9,
        TextBlockConvertFlags(flags));
    TextBlockNDCToDDC(*extent, 0.0f, extent, nullptr);
    return result;
}

// ref: FUN_141285460
uint32_t TextBlockGetMaxBytesWithinHeight(const HTEXTFONT& font, const char* text, float height, float width,
    float maxHeight, int32_t lines, float indent, float scale, float spacing, uint32_t flags) {
    float ndcSpacing = TextBlockDDCToNDCHeight(spacing);
    float ndcMaxHeight = TextBlockDDCToNDCHeight(maxHeight);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    return GxuFontGetMaxBytesWithinHeight(font->m_font, text, ndcHeight, ndcWidth, ndcMaxHeight, lines, indent, scale,
        ndcSpacing, TextBlockConvertFlags(flags));
}

// ref: FUN_141285540
uint32_t TextBlockGetMaxCharsWithinWidth(const HTEXTFONT& font, const char* text, float height, float width,
    uint32_t count, float* extent, float a7, float scale, float a9, uint32_t flags) {
    float ndcA9 = TextBlockDDCToNDCWidth(a9);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    uint32_t result = GxuFontGetMaxCharsWithinWidth(font->m_font, text, ndcHeight, ndcWidth, count, extent, a7, scale,
        ndcA9, TextBlockConvertFlags(flags));
    TextBlockNDCToDDC(*extent, 0.0f, extent, nullptr);
    return result;
}

// ref: FUN_141285690
void TextBlockGetWrappedWidth(const HTEXTFONT& font, const char* text, uint32_t length, float height, float* extent,
    float a6, float scale, float a8, uint32_t flags) {
    *extent = 0.0f;
    float ndcA8 = TextBlockDDCToNDCWidth(a8);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    GxuFontGetWrappedWidth(font->m_font, text, length, ndcHeight, extent, a6, scale, ndcA8,
        TextBlockConvertFlags(flags));
    TextBlockNDCToDDC(*extent, 0.0f, extent, nullptr);
}

// ref: FUN_141285770
float TextBlockGetWrappedTextHeight(const HTEXTFONT& font, const char* text, float height, float width,
    const float* indent, float scale, float spacing, uint32_t flags) {
    float ndcSpacing = TextBlockDDCToNDCHeight(spacing);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    float result = GxuFontGetWrappedTextHeight(font->m_font, text, ndcHeight, ndcWidth, indent, scale, ndcSpacing,
        TextBlockConvertFlags(flags));
    return TextBlockNDCToDDCHeight(result);
}

// ref: FUN_141285950
uint32_t TextBlockGetLineStarts(const HTEXTFONT& font, const char* text, float height, float width, int32_t* offsets,
    uint32_t maxOffsets, float a7, float scale, float a9, uint32_t flags) {
    float ndcA9 = TextBlockDDCToNDCWidth(a9);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    uint32_t gxuFlags = TextBlockConvertFlags(flags);
    uint32_t length = static_cast<uint32_t>(strlen(text));
    return GxuFontGetLineStarts(font->m_font, text, length, ndcHeight, ndcWidth, offsets, maxOffsets, a7, scale, ndcA9,
        gxuFlags);
}

// ---- text blocks -----------------------------------------------------------------------------

CTextBlock::~CTextBlock() {
    GxuFontDestroyString(&this->m_string);
}

// ref: FUN_141285020
HTEXTBLOCK TextBlockCreate(const HTEXTFONT& font, const char* text, const uint32_t* color, const float* position,
    float height, float width, float maxHeight, uint32_t flags, float a9, float spacing, float textScale, float scale,
    float rotation) {
    FONTHASHOBJ* object = font.get();
    void* memory = SMemAlloc(sizeof(CTextBlock), 1, "HTEXTBLOCK", 0, -2, 0);
    auto block = new (memory) CTextBlock();

    float ndcPosition[3];
    ndcPosition[2] = position[2];
    TextBlockDDCToNDC(position[0], position[1], &ndcPosition[0], &ndcPosition[1]);
    float ndcA9 = TextBlockDDCToNDCWidth(a9);
    uint32_t gxuFlags = TextBlockConvertFlags(flags);

    int32_t justifyH;
    if ((flags & TEXTBLOCK_JUSTIFY_RIGHT) != 0) {
        justifyH = 2;
    } else if ((flags & TEXTBLOCK_JUSTIFY_CENTER) != 0) {
        justifyH = 1;
    } else {
        justifyH = (flags & TEXTBLOCK_JUSTIFY_LEFT) != 0 ? 0 : 1;
    }

    int32_t justifyV = 0;
    if ((flags & TEXTBLOCK_JUSTIFY_TOP) == 0) {
        justifyV = (flags & TEXTBLOCK_JUSTIFY_BOTTOM) != 0 ? 2 : 1;
    }

    float ndcSpacing = TextBlockDDCToNDCHeight(spacing);
    float ndcMaxHeight = TextBlockDDCToNDCHeight(maxHeight);
    float ndcWidth = TextBlockDDCToNDCWidth(width);
    float ndcHeight = TextBlockDDCToNDCHeight(height);
    GxuFontCreateString(object->m_font, text, ndcHeight, ndcPosition, ndcWidth, ndcMaxHeight, ndcSpacing,
        &block->m_string, justifyV, justifyH, gxuFlags, color, ndcA9, textScale, scale, rotation);
    return static_cast<HTEXTBLOCK>(HandleDuplicate(block));
}

// ref: FUN_141285000
void TextBlockSetShadow(HTEXTBLOCK block, uint32_t color, const float* offset) {
    GxuFontStringSetShadow(block->m_string, &color, offset);
}

// ref: FUN_141285840
void TextBlockClearShadow(HTEXTBLOCK block) {
    GxuFontStringClearShadow(block->m_string);
}

// ref: FUN_141285850
int32_t TextBlockSetGradient(HTEXTBLOCK block, int32_t start, int32_t length) {
    return GxuFontStringSetGradient(block->m_string, start, length);
}

// ref: FUN_141285860
void TextBlockSetStringPos(HTEXTBLOCK block, const float* position) {
    float ndcPosition[3];
    ndcPosition[2] = position[2];
    TextBlockDDCToNDC(position[0], position[1], &ndcPosition[0], &ndcPosition[1]);
    block->m_string->SetPosition(ndcPosition);
}

// ref: FUN_141285640
void TextBlockGetStringPos(float* position, HTEXTBLOCK block) {
    float ndcPosition[3];
    GxuFontStringGetPosition(ndcPosition, block->m_string);
    position[2] = ndcPosition[2];
    TextBlockNDCToDDC(ndcPosition[0], ndcPosition[1], &position[0], &position[1]);
}

// ref: FUN_141285940
void TextBlockUpdateColor(HTEXTBLOCK block, const uint32_t* color) {
    GxuFontStringSetColor(block->m_string, color);
}
