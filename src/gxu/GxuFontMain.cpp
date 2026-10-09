#include "gxu/GxuFont.hpp"

#include "util/Unimplemented.hpp"

namespace {

GXUCOLORNAMELOOKUP s_colorNameLookup;   // 0x142e9cf80

}

// ref: FUN_14068b740
void GxuFontSetColorNameLookup(GXUCOLORNAMELOOKUP lookup) {
    s_colorNameLookup = lookup;
}

// ref: FUN_14068ac10
GXUCOLORNAMELOOKUP GxuFontGetColorNameLookup() {
    return s_colorNameLookup;
}

// ref: FUN_14068a390
GxuStringBatch* GxuStringBatchCreate() {
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14068ab40
void GxuStringBatchDestroy(GxuStringBatch* batch) {
    (void)batch;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14068a1e0
void GxuStringBatchAddString(GxuStringBatch* batch, void* string) {
    (void)batch;
    (void)string;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14068b6c0
void GxuStringBatchRender(GxRenderContext* context, GxuStringBatch* batch, const float* view, const float* projection,
    bool a5) {
    (void)context;
    (void)batch;
    (void)view;
    (void)projection;
    (void)a5;
    FROST_UNIMPLEMENTED();
}
