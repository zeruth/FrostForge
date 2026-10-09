#ifndef GXU_GXU_FONT_HPP
#define GXU_GXU_FONT_HPP

// Gxu fonts (Engine/Source/Gxu/GxuFontMain.cpp): text rendering on top of Gx.

#include <cstdint>

// Resolves a named colour (|cn<name>: in text, color="<name>" in FrameXML) to BGRA bytes;
// sets *found.
typedef const uint8_t* (*GXUCOLORNAMELOOKUP)(const char* name, bool* found);

void GxuFontSetColorNameLookup(GXUCOLORNAMELOOKUP lookup);
GXUCOLORNAMELOOKUP GxuFontGetColorNameLookup();

class GxRenderContext;
struct GxuStringBatch;

// The batches font strings draw through: made (FUN_14068a390), handed back (FUN_14068ab40),
// given a string (FUN_14068a1e0) and drawn with the view and projection (FUN_14068b6c0).
GxuStringBatch* GxuStringBatchCreate();
void GxuStringBatchDestroy(GxuStringBatch* batch);
void GxuStringBatchAddString(GxuStringBatch* batch, void* string);
void GxuStringBatchRender(GxRenderContext* context, GxuStringBatch* batch, const float* view, const float* projection,
    bool a5);

#endif
