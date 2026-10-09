#include "gxu/GxuFont.hpp"

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
