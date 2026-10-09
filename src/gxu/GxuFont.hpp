#ifndef GXU_GXU_FONT_HPP
#define GXU_GXU_FONT_HPP

// Gxu fonts (Engine/Source/Gxu/GxuFontMain.cpp): text rendering on top of Gx.

#include <cstdint>

// Resolves a named colour (|cn<name>: in text, color="<name>" in FrameXML) to BGRA bytes;
// sets *found.
typedef const uint8_t* (*GXUCOLORNAMELOOKUP)(const char* name, bool* found);

void GxuFontSetColorNameLookup(GXUCOLORNAMELOOKUP lookup);
GXUCOLORNAMELOOKUP GxuFontGetColorNameLookup();

#endif
