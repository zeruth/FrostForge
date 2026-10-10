#ifndef UI_UI_TYPES_HPP
#define UI_UI_TYPES_HPP

// The enumerations FrameXML attributes and the Lua API name by string: anchor points, draw
// layers, strata, justification, font flags, blend modes, wrap modes, orientation and fill
// style, with the conversions each way.

#include <cstdint>

enum FRAMEPOINT : uint32_t {
    FRAMEPOINT_TOPLEFT = 0,
    FRAMEPOINT_TOP = 1,
    FRAMEPOINT_TOPRIGHT = 2,
    FRAMEPOINT_LEFT = 3,
    FRAMEPOINT_CENTER = 4,
    FRAMEPOINT_RIGHT = 5,
    FRAMEPOINT_BOTTOMLEFT = 6,
    FRAMEPOINT_BOTTOM = 7,
    FRAMEPOINT_BOTTOMRIGHT = 8,
    FRAMEPOINT_NUMPOINTS = 9,
};

enum DRAWLAYER : uint32_t {
    DRAWLAYER_BACKGROUND = 0,
    DRAWLAYER_BORDER = 1,
    DRAWLAYER_ARTWORK = 2,
    DRAWLAYER_OVERLAY = 3,
    DRAWLAYER_HIGHLIGHT = 4,
};

enum FRAME_STRATA : uint32_t {
    FRAME_STRATA_WORLD = 0,
    FRAME_STRATA_BACKGROUND = 1,
    FRAME_STRATA_LOW = 2,
    FRAME_STRATA_MEDIUM = 3,
    FRAME_STRATA_HIGH = 4,
    FRAME_STRATA_DIALOG = 5,
    FRAME_STRATA_FULLSCREEN = 6,
    FRAME_STRATA_FULLSCREEN_DIALOG = 7,
    FRAME_STRATA_TOOLTIP = 8,
    FRAME_STRATA_BLIZZARD = 9,
};

// The XML/Lua truth test: "0", "F...", "N...", "off", "disabled" are false; a digit, "T...",
// "Y...", "on" and "enabled" are true; anything else is false.
bool StringToBOOL(const char* string);

bool StringToWrapMode(const char* string, uint8_t* mode);
bool StringToJustify(const char* string, uint32_t* justify);
const char* JustifyToString(uint32_t justify);
// The flags named anywhere in `string` (OUTLINE, THICKOUTLINE, MONOCHROME).
uint8_t* StringToFontFlags(uint8_t* flags, const char* string);
// "OUTLINE, MONOCHROME", ... in a shared buffer.
const char* FontFlagsToString(uint8_t flags);
bool IsValidFontFlags(const char* string);
bool StringToBlendMode(const char* string, uint8_t* mode);
const char* BlendModeToString(uint8_t mode);
bool StringToDrawLayer(const char* string, uint32_t* layer);
const char* DrawLayerToString(uint32_t layer);
bool StringToFramePoint(const char* string, uint32_t* point);
const char* FramePointToString(uint32_t point);
bool StringToFrameStrata(const char* string, uint32_t* strata);
const char* FrameStrataToString(uint32_t strata);
bool StringToOrientation(const char* string, uint32_t* orientation);
const char* OrientationToString(uint32_t orientation);
bool StringToFillStyle(const char* string, uint8_t* style);
const char* FillStyleToString(uint8_t style);

// Whether the UI may look its art and data up in the client databases (atlas members, file
// data); set once the glue or game UI starts (0x142e97a90).
extern bool g_uiDatabaseReady;
void UiSetDatabaseReady();

#endif
