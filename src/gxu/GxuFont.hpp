#ifndef GXU_GXU_FONT_HPP
#define GXU_GXU_FONT_HPP

// Gxu fonts (Engine/Source/Gxu/GxuFontMain.cpp): text rendering on top of Gx. Fonts are a face
// at a height with a glyph cache; strings are text laid out with a font; batches draw strings.

#include <cstdint>

class GxRenderContext;
class CGxuFont;
struct CGxString;
class GxuStringBatch;
struct GXUHYPERLINKINFO;
struct GXUEMBEDDEDTEXTURE;

// Resolves a named colour (|cn<name>: in text, color="<name>" in FrameXML) to BGRA bytes;
// sets *found.
typedef const uint8_t* (*GXUCOLORNAMELOOKUP)(const char* name, bool* found);

// String flags (CGxString::m_flags).
enum : uint32_t {
    GXU_STRING_SHADOW = 0x1,
    GXU_STRING_SINGLE_LINE = 0x2,
    GXU_STRING_ONE_TO_ONE = 0x4,
    GXU_STRING_NO_COLOR_CODES = 0x8,
    GXU_STRING_MONOSPACE = 0x10,
    GXU_STRING_GRADIENT = 0x20,
    GXU_STRING_BREAK_ANYWHERE = 0x40,
    GXU_STRING_WORLD = 0x80,
    GXU_STRING_INDENT = 0x2000,
};

// ---- the module ------------------------------------------------------------------------------

// Starts the module at a window size: device and settings callbacks, FreeType (FUN_14068b4c0).
void GxuFontInitialize(const uint32_t* size);
// A new window size: fonts are rebuilt for it (FUN_14068bc90).
void GxuFontWindowSizeChanged(const uint32_t* size);
// Takes the module down: strings, fonts, faces and FreeType (FUN_14068b7c0).
void GxuFontDestroy();
// Drops geometry nobody drew lately and starts a new eviction period (FUN_14068bc30).
void GxuFontUpdate();
// Drops the glyphs of fonts unused for sixty frames (FUN_14068aae0).
void GxuFontReleaseIdleFonts();
// Registers the module's empty device callbacks (FUN_14068c520).
void GxuFontRegisterCallbacks();

void GxuFontSetColorNameLookup(GXUCOLORNAMELOOKUP lookup);   // FUN_14068b740
GXUCOLORNAMELOOKUP GxuFontGetColorNameLookup();              // FUN_14068ac10
void GxuFontSetStringRenderer(void* renderer);              // FUN_14068b7b0
void* GxuFontGetStringRenderer();                            // FUN_14068b240
float GxuFontGetIndent();                                    // FUN_14068ac60

// ---- fonts -----------------------------------------------------------------------------------

// A font from a file data id or path at a height (FUN_14068a5c0, FUN_14068a6d0); returns
// whether it loaded.
bool GxuFontCreateFont(int32_t fileDataId, float height, CGxuFont** font, uint32_t flags);
bool GxuFontCreateFontByName(const char* path, float height, CGxuFont** font, uint32_t flags);
void GxuFontDestroyFont(CGxuFont** font);                   // FUN_14068ab80
int32_t GxuFontGetFileDataId(CGxuFont* font);               // FUN_14068ac20
uint32_t GxuFontGetFlags(CGxuFont* font);                   // FUN_14068ac30
uint32_t GxuFontGetPixelSize(CGxuFont* font);               // FUN_14068b1b0
const char* GxuFontGetFontName(CGxuFont* font);             // FUN_14068ac40, the path it was made from
void GxuFontSetFlag40(CGxuFont* font);                      // FUN_14068b750
float GxuFontGetOneToOneHeight(CGxuFont* font);             // FUN_14068b180
float GxuFontGetEffectiveHeight(CGxuFont* font, float height, uint32_t flags);  // FUN_14068ac70
// The pixel size a face already loaded would get (1), or 2 when it is not loaded
// (FUN_14068a2d0, FUN_14068a320).
int32_t GxuFontGetFacePixelSize(uint32_t fileDataId, float height, uint32_t flags, uint32_t* pixelSize);
int32_t GxuFontGetFacePixelSizeByName(const char* path, float height, uint32_t flags, uint32_t* pixelSize);
// A face over bytes already loaded, by the file's name (FUN_14068b630).
void* GxuFontCreateFaceFromBuffer(const char* path, void* data, uint32_t size);

// ---- measuring -------------------------------------------------------------------------------

// The width of up to `length` bytes on one line and how many characters show (FUN_14068acb0).
int32_t GxuFontGetTextExtent(CGxuFont* font, const char* text, float height, float width, uint32_t length,
    float* extent, float indent, float scale, float unused, uint32_t flags);
// The widest line of up to `length` bytes (FUN_14068b210).
void GxuFontGetWrappedWidth(CGxuFont* font, const char* text, uint32_t length, float height, float* extent,
    float indent, float scale, float unused, uint32_t flags);
// The height a string wraps to (FUN_14068b250).
float GxuFontGetWrappedTextHeight(CGxuFont* font, const char* text, float height, float width, const float* indent,
    float scale, float spacing, uint32_t flags);
// How many bytes fit in `maxHeight` (FUN_14068ad10).
uint32_t GxuFontGetMaxBytesWithinHeight(CGxuFont* font, const char* text, float height, float width, float maxHeight,
    int32_t lines, float indent, float scale, float spacing, uint32_t flags);
// How many trailing characters fit in `width` (FUN_14068afc0).
uint32_t GxuFontGetMaxCharsWithinWidth(CGxuFont* font, const char* text, float height, float width, uint32_t count,
    float* extent, float indent, float scale, float unused, uint32_t flags);
// Each line's starting offset (FUN_14068bda0); returns the line count.
uint32_t GxuFontGetLineStarts(CGxuFont* font, const char* text, uint32_t length, float height, float width,
    int32_t* offsets, uint32_t maxOffsets, float indent, float scale, float unused, uint32_t flags);
// The control code at `text` (FUN_140689b30).
int32_t GxuDetermineControlCode(const char* text, int32_t* advance, uint32_t* color, uint32_t flags, uint32_t* code);
// Moves *position back over a |T or |A code or a UTF-8 sequence (FUN_14068a210).
void GxuFontBackUpOverCode(const char* text, uint32_t* position);
// Whether a line may break between two characters (FUN_14068f500).
bool GxuFontCanBreakBetween(uint32_t previous, uint32_t code);
// Copies text without the codes `mask` names (FUN_14068ba40).
char* GxuFontStripCodes(const char* text, int32_t length, uint32_t mask, char* out, uint32_t outSize);

// ---- strings ---------------------------------------------------------------------------------

// A string laid out with a font (FUN_14068a7e0); returns whether it was made.
bool GxuFontCreateString(CGxuFont* font, const char* text, float height, const float* position, float width,
    float maxHeight, float spacing, CGxString** string, int32_t justifyV, int32_t justifyH, uint32_t flags,
    const uint32_t* color, float unused, float textScale, float scale, float rotation);
void GxuFontDestroyString(CGxString** string);              // FUN_14068abd0
void GxuFontStringSetShadow(CGxString* string, const uint32_t* color, const float* offset);  // FUN_14068a1c0
void GxuFontStringClearShadow(CGxString* string);           // FUN_14068b690
bool GxuFontStringSetColor(CGxString* string, const uint32_t* color);  // FUN_14068b780
void GxuFontStringSetScale(CGxString* string, float scale);         // FUN_14068b770
void GxuFontStringSetRotation(CGxString* string, float rotation);   // FUN_14068b760
int32_t GxuFontStringSetGradient(CGxString* string, int32_t start, int32_t length);  // FUN_14068ba30
void GxuFontStringGetPosition(float* position, CGxString* string);  // FUN_14068b1c0
int32_t GxuFontStringGetHyperlinks(CGxString* string, GXUHYPERLINKINFO** links);   // FUN_14068b9c0
int32_t GxuFontStringGetTextures(CGxString* string, GXUEMBEDDEDTEXTURE** textures);  // FUN_14068b9b0
bool GxuFontStringRemoveFromBatch(CGxString* string);       // FUN_14068b650

// ---- batches ---------------------------------------------------------------------------------

// The batches font strings draw through: made (FUN_14068a390, FUN_14068a480 with flags),
// handed back (FUN_14068ab40), cleared (FUN_14068a370), given a string (FUN_14068a1e0) and
// drawn with the view and projection (FUN_14068b6c0).
GxuStringBatch* GxuStringBatchCreate();
GxuStringBatch* GxuStringBatchCreateWithFlags(bool world, bool a2, bool a3, bool a4);
bool GxuStringBatchDestroy(GxuStringBatch* batch);
bool GxuStringBatchClear(GxuStringBatch* batch);
bool GxuStringBatchAddString(GxuStringBatch* batch, CGxString* string);
void GxuStringBatchRender(GxRenderContext* context, GxuStringBatch* batch, const float* view, const float* projection,
    const float* depth);

#endif
