#ifndef GXU_TEXT_BLOCK_HPP
#define GXU_TEXT_BLOCK_HPP

// Text blocks: the frame system's view of Gxu fonts. Fonts are shared through a cache keyed
// by file, flags and pixel size and handed out as blz::shared_ptr<FONTHASHOBJ>; a text block
// (HTEXTBLOCK) is a Gxu string made with one. Coordinates and heights here are in the frame
// system's units and are converted on the way through by the screen's diagonal (0.8 across,
// 0.6 up).

#include <cstdint>

#include "blz/SharedPtr.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/hash/TSHashTable.hpp"

class CGxuFont;
struct CGxString;

// Text block font flags (what FrameXML's outline/monochrome/thick attributes become).
enum : uint8_t {
    TEXTBLOCK_FONT_OUTLINE = 0x1,
    TEXTBLOCK_FONT_MONOCHROME = 0x2,
    TEXTBLOCK_FONT_THICK = 0x4,
    TEXTBLOCK_FONT_8 = 0x8,
    TEXTBLOCK_FONT_FIXED_SIZE = 0x10,   // the height is pixels, not frame units
    TEXTBLOCK_FONT_20 = 0x20,           // sets the font's 0x40 flag; not part of the key
};

// Text block string flags: justification in the low bits, then the Gxu string flags.
enum : uint32_t {
    TEXTBLOCK_JUSTIFY_LEFT = 0x1,
    TEXTBLOCK_JUSTIFY_CENTER = 0x2,
    TEXTBLOCK_JUSTIFY_RIGHT = 0x4,
    TEXTBLOCK_JUSTIFY_TOP = 0x8,
    TEXTBLOCK_JUSTIFY_MIDDLE = 0x10,
    TEXTBLOCK_JUSTIFY_BOTTOM = 0x20,
    TEXTBLOCK_SINGLE_LINE = 0x40,
    TEXTBLOCK_WORLD = 0x80,
    TEXTBLOCK_SHADOW = 0x100,
    TEXTBLOCK_ONE_TO_ONE = 0x200,
    TEXTBLOCK_NO_COLOR_CODES = 0x400,
    TEXTBLOCK_MONOSPACE = 0x800,
    TEXTBLOCK_BREAK_ANYWHERE = 0x1000,
    TEXTBLOCK_INDENT = 0x20000,
};

// The cache's key (0x10): a file, the flags and the pixel size the face would get. The
// inserted node keeps the font itself where the lookup key has the pixel size.
struct FontHashKey {
    int32_t m_fileDataId;           // 0x30
    uint8_t m_flags;                // 0x34
    uint8_t m_pad35[3];
    union {
        uint32_t m_pixelSize;       // 0x38 looking up
        CGxuFont* m_font;           // 0x38 stored
    };
};
static_assert(sizeof(FontHashKey) == 0x10, "FontHashKey is 0x10 bytes");

// A cached font (0x58): the node, a weak reference to itself to hand out, and the font.
struct FONTHASHOBJ {
    GXU_HASH_OBJECT(FONTHASHOBJ)            // 0x00
    uint32_t m_pad2c = 0;
    FontHashKey m_key = {};                 // 0x30
    blz::weak_ptr<FONTHASHOBJ> m_self;      // 0x40
    CGxuFont* m_font = nullptr;             // 0x50
};
static_assert(sizeof(FONTHASHOBJ) == 0x58, "FONTHASHOBJ is 0x58 bytes");

typedef blz::shared_ptr<FONTHASHOBJ> HTEXTFONT;

// A text block (0x18, a handle): the string.
class CTextBlock : public CHandleObject {
    public:
    CGxString* m_string = nullptr;  // 0x10

    // ref: FUN_141284540
    ~CTextBlock() override;
};
typedef CTextBlock* HTEXTBLOCK;

// ---- units -----------------------------------------------------------------------------------

// Frame units to Gxu units and back (FUN_141284590, FUN_1412845c0, FUN_1412845d0,
// FUN_141284cd0, FUN_141284d00).
void TextBlockDDCToNDC(float x, float y, float* outX, float* outY);
float TextBlockDDCToNDCHeight(float height);
float TextBlockDDCToNDCWidth(float width);
void TextBlockNDCToDDC(float x, float y, float* outX, float* outY);
float TextBlockNDCToDDCHeight(float height);
// Text block string flags to Gxu string flags (FUN_141284710).
uint32_t TextBlockConvertFlags(uint32_t flags);

// ---- fonts -----------------------------------------------------------------------------------

// The cached font of a file at a height, made when there is none (FUN_141283fd0, by path
// FUN_1412841c0). Empty when the face cannot be made.
HTEXTFONT TextBlockGenerateFont(int32_t fileDataId, uint8_t flags, float height);
HTEXTFONT TextBlockGenerateFont(const char* path, uint8_t flags, float height);
// Drops every cached font (FUN_1412858c0).
void TextBlockDestroyFonts();

int32_t TextBlockGetFontFileDataId(const HTEXTFONT& font);      // FUN_141285280
uint8_t TextBlockGetFontFlags(const HTEXTFONT& font);           // FUN_141285290
const char* TextBlockGetFontName(const HTEXTFONT& font);        // FUN_1412852f0
CGxuFont* TextBlockGetFontPtr(const HTEXTFONT& font);           // FUN_141285300
// The height a string of the font really gets (FUN_141285310).
float TextBlockGetFontHeight(const HTEXTFONT& font, float height, uint32_t flags);

// ---- measuring -------------------------------------------------------------------------------

int32_t TextBlockGetTextExtent(const HTEXTFONT& font, const char* text, float height, float width, uint32_t length,
    float* extent, float a7, float scale, float a9, uint32_t flags);                    // FUN_141285360
uint32_t TextBlockGetMaxBytesWithinHeight(const HTEXTFONT& font, const char* text, float height, float width,
    float maxHeight, int32_t lines, float indent, float scale, float spacing, uint32_t flags);  // FUN_141285460
uint32_t TextBlockGetMaxCharsWithinWidth(const HTEXTFONT& font, const char* text, float height, float width,
    uint32_t count, float* extent, float a7, float scale, float a9, uint32_t flags);    // FUN_141285540
void TextBlockGetWrappedWidth(const HTEXTFONT& font, const char* text, uint32_t length, float height, float* extent,
    float a6, float scale, float a8, uint32_t flags);                                   // FUN_141285690
float TextBlockGetWrappedTextHeight(const HTEXTFONT& font, const char* text, float height, float width,
    const float* indent, float scale, float spacing, uint32_t flags);                  // FUN_141285770
uint32_t TextBlockGetLineStarts(const HTEXTFONT& font, const char* text, float height, float width, int32_t* offsets,
    uint32_t maxOffsets, float a7, float scale, float a9, uint32_t flags);              // FUN_141285950

// ---- text blocks -----------------------------------------------------------------------------

// A string of `text` in the font (FUN_141285020).
HTEXTBLOCK TextBlockCreate(const HTEXTFONT& font, const char* text, const uint32_t* color, const float* position,
    float height, float width, float maxHeight, uint32_t flags, float a9, float spacing, float textScale, float scale,
    float rotation);
void TextBlockSetShadow(HTEXTBLOCK block, uint32_t color, const float* offset);  // FUN_141285000
void TextBlockClearShadow(HTEXTBLOCK block);                     // FUN_141285840
int32_t TextBlockSetGradient(HTEXTBLOCK block, int32_t start, int32_t length);  // FUN_141285850
void TextBlockSetStringPos(HTEXTBLOCK block, const float* position);  // FUN_141285860
void TextBlockGetStringPos(float* position, HTEXTBLOCK block);   // FUN_141285640
void TextBlockUpdateColor(HTEXTBLOCK block, const uint32_t* color);  // FUN_141285940

#endif
