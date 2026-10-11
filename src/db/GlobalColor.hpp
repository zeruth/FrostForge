#ifndef DB_GLOBAL_COLOR_HPP
#define DB_GLOBAL_COLOR_HPP

// The named colours of GlobalColor.db2 (NORMAL_FONT_COLOR, TRIVIAL_DIFFICULTY_COLOR, ...), read
// once into a name -> colour map the first time one is asked for. XML <Color name="..."/> and
// the font's |cn<name>: escape look them up through the callback GxuFont keeps
// (GxuFontSetColorNameLookup).

#include <cstdint>

// A colour: its 0xAARRGGBB value, and that value as hex text ("%x", at most 8 digits).
struct COLORINFO {
    uint32_t m_color;   // 0x00
    char m_hex[12];     // 0x04
};

// The colour named `name`, or null; `found` (when given) says which.
const COLORINFO* GlobalColorFind(const char* name, bool* found);

// GlobalColorFind, with magenta for a name the table does not have: the callback GxuFont is
// given (a GXUCOLORNAMELOOKUP, so the COLORINFO as its bytes, B G R A); `found` still says
// whether the name was there.
const uint8_t* GlobalColorLookup(const char* name, bool* found);

#endif
