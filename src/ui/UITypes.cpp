#include "ui/UITypes.hpp"

#include <cstring>

#include "storm/String.hpp"

namespace {

template <typename T>
struct ENUMNAME {
    T m_value;
    const char* m_name;
};

// 0x1426cef30
const ENUMNAME<uint8_t> s_wrapModes[] = {
    { 0, "CLAMP" },
    { 1, "REPEAT" },
    { 2, "CLAMPTOBLACK" },
    { 3, "CLAMPTOBLACKADDITIVE" },
    { 4, "CLAMPTOWHITE" },
    { 5, "MIRROR" },
};

// 0x1426d09b0
const ENUMNAME<uint32_t> s_justify[] = {
    { 0x01, "LEFT" },
    { 0x02, "CENTER" },
    { 0x04, "RIGHT" },
    { 0x08, "TOP" },
    { 0x10, "MIDDLE" },
    { 0x20, "BOTTOM" },
};

// 0x1426d0a28
const ENUMNAME<uint8_t> s_fontFlags[] = {
    { 1, "OUTLINE" },
    { 4, "THICKOUTLINE" },
    { 2, "MONOCHROME" },
};

// 0x1426d0ae0
const ENUMNAME<uint8_t> s_blendModes[] = {
    { 0, "DISABLE" },
    { 2, "BLEND" },
    { 1, "ALPHAKEY" },
    { 3, "ADD" },
    { 4, "MOD" },
};

// 0x1426d0b50
const ENUMNAME<uint32_t> s_drawLayers[] = {
    { DRAWLAYER_BACKGROUND, "BACKGROUND" },
    { DRAWLAYER_BORDER, "BORDER" },
    { DRAWLAYER_ARTWORK, "ARTWORK" },
    { DRAWLAYER_OVERLAY, "OVERLAY" },
    { DRAWLAYER_HIGHLIGHT, "HIGHLIGHT" },
};

// 0x142cf5950
const char* const s_framePoints[FRAMEPOINT_NUMPOINTS] = {
    "TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT", "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT",
};

// 0x1426d0c10
const ENUMNAME<uint32_t> s_strata[] = {
    { FRAME_STRATA_WORLD, "WORLD" },
    { FRAME_STRATA_BACKGROUND, "BACKGROUND" },
    { FRAME_STRATA_LOW, "LOW" },
    { FRAME_STRATA_MEDIUM, "MEDIUM" },
    { FRAME_STRATA_HIGH, "HIGH" },
    { FRAME_STRATA_DIALOG, "DIALOG" },
    { FRAME_STRATA_FULLSCREEN, "FULLSCREEN" },
    { FRAME_STRATA_FULLSCREEN_DIALOG, "FULLSCREEN_DIALOG" },
    { FRAME_STRATA_TOOLTIP, "TOOLTIP" },
    { FRAME_STRATA_BLIZZARD, "BLIZZARD" },
};

// 0x1426d0d18
const ENUMNAME<uint32_t> s_orientations[] = {
    { 0, "HORIZONTAL" },
    { 1, "VERTICAL" },
};

// 0x1426d0d60
const ENUMNAME<uint8_t> s_fillStyles[] = {
    { 0, "STANDARD" },
    { 1, "STANDARD_NO_RANGE_FILL" },
    { 2, "CENTER" },
    { 3, "REVERSE" },
};

char s_fontFlagsText[0x40];  // 0x142e9a2c0

// The bounded append FontFlagsToString inlines: stops one short of the buffer's end.
void AppendFontFlagsText(const char* text) {
    char* dest = s_fontFlagsText;
    uint64_t left = sizeof(s_fontFlagsText);
    while (left != 0 && *dest != '\0') {
        dest++;
        left--;
    }
    left = s_fontFlagsText + sizeof(s_fontFlagsText) - dest;
    if (left == 0) {
        return;
    }
    for (; *text != '\0'; text++) {
        if (left != 1) {
            *dest++ = *text;
            left--;
        }
    }
    *dest = '\0';
}

}

// ref: FUN_1406689d0
bool StringToBOOL(const char* string) {
    if (string == nullptr) {
        return false;
    }
    switch (*string) {
        case '0':
        case 'F':
        case 'N':
        case 'f':
        case 'n':
            return false;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        case 'T':
        case 'Y':
        case 't':
        case 'y':
            return true;
        default:
            if (_stricmp(string, "off") == 0 || _stricmp(string, "disabled") == 0) {
                return false;
            }
            return _stricmp(string, "on") == 0 || _stricmp(string, "enabled") == 0;
    }
}

// ref: FUN_140668ac0
bool StringToWrapMode(const char* string, uint8_t* mode) {
    for (const auto& entry : s_wrapModes) {
        if (_stricmp(entry.m_name, string) == 0) {
            *mode = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_140672050
bool StringToJustify(const char* string, uint32_t* justify) {
    for (const auto& entry : s_justify) {
        if (_stricmp(entry.m_name, string) == 0) {
            *justify = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_140672010
const char* JustifyToString(uint32_t justify) {
    for (const auto& entry : s_justify) {
        if (entry.m_value & justify) {
            return entry.m_name;
        }
    }
    return "UNKNOWN";
}

// ref: FUN_140672290
uint8_t* StringToFontFlags(uint8_t* flags, const char* string) {
    *flags = 0;
    if (SStrStrI(string, s_fontFlags[0].m_name)) {
        *flags |= 1;
    }
    if (SStrStrI(string, s_fontFlags[1].m_name)) {
        *flags |= 4;
    }
    if (SStrStrI(string, s_fontFlags[2].m_name)) {
        *flags |= 2;
    }
    return flags;
}

// ref: FUN_1406720c0
const char* FontFlagsToString(uint8_t flags) {
    s_fontFlagsText[0] = '\0';
    if (flags & 1) {
        AppendFontFlagsText(s_fontFlags[0].m_name);
    }
    if ((s_fontFlags[1].m_value & flags) == s_fontFlags[1].m_value) {
        if (s_fontFlagsText[0] != '\0') {
            AppendFontFlagsText(", ");
        }
        AppendFontFlagsText(s_fontFlags[1].m_name);
    }
    if ((s_fontFlags[2].m_value & flags) == s_fontFlags[2].m_value) {
        if (s_fontFlagsText[0] != '\0') {
            AppendFontFlagsText(", ");
        }
        AppendFontFlagsText(s_fontFlags[2].m_name);
    }
    return s_fontFlagsText;
}

// ref: FUN_140672300
bool IsValidFontFlags(const char* string) {
    if (*string == '\0') {
        return true;
    }
    for (const auto& entry : s_fontFlags) {
        if (SStrStrI(string, entry.m_name)) {
            return true;
        }
    }
    return false;
}

// ref: FUN_1406723a0
bool StringToBlendMode(const char* string, uint8_t* mode) {
    for (const auto& entry : s_blendModes) {
        if (_stricmp(entry.m_name, string) == 0) {
            *mode = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_140672360
const char* BlendModeToString(uint8_t mode) {
    for (const auto& entry : s_blendModes) {
        if (entry.m_value == mode) {
            return entry.m_name;
        }
    }
    return nullptr;
}

// ref: FUN_140672450
bool StringToDrawLayer(const char* string, uint32_t* layer) {
    for (const auto& entry : s_drawLayers) {
        if (_stricmp(entry.m_name, string) == 0) {
            *layer = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_140672410
const char* DrawLayerToString(uint32_t layer) {
    for (const auto& entry : s_drawLayers) {
        if (entry.m_value == layer) {
            return entry.m_name;
        }
    }
    return "UNKNOWN";
}

// ref: FUN_1406724e0
bool StringToFramePoint(const char* string, uint32_t* point) {
    for (uint32_t i = 0; i < FRAMEPOINT_NUMPOINTS; i++) {
        if (_stricmp(s_framePoints[i], string) == 0) {
            *point = i;
            return true;
        }
    }
    return false;
}

// ref: FUN_1406724c0
const char* FramePointToString(uint32_t point) {
    if (static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(point))) < FRAMEPOINT_NUMPOINTS) {
        return s_framePoints[point];
    }
    return "UNKNOWN";
}

// ref: FUN_140672580
bool StringToFrameStrata(const char* string, uint32_t* strata) {
    for (const auto& entry : s_strata) {
        if (_stricmp(entry.m_name, string) == 0) {
            *strata = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_140672540
const char* FrameStrataToString(uint32_t strata) {
    for (const auto& entry : s_strata) {
        if (entry.m_value == strata) {
            return entry.m_name;
        }
    }
    return "UNKNOWN";
}

// ref: FUN_140672630
bool StringToOrientation(const char* string, uint32_t* orientation) {
    for (const auto& entry : s_orientations) {
        if (_stricmp(entry.m_name, string) == 0) {
            *orientation = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_1406725f0
const char* OrientationToString(uint32_t orientation) {
    for (const auto& entry : s_orientations) {
        if (entry.m_value == orientation) {
            return entry.m_name;
        }
    }
    return "UNKNOWN";
}

// ref: FUN_1406726d0
bool StringToFillStyle(const char* string, uint8_t* style) {
    for (const auto& entry : s_fillStyles) {
        if (_stricmp(entry.m_name, string) == 0) {
            *style = entry.m_value;
            return true;
        }
    }
    return false;
}

// ref: FUN_1406726a0
const char* FillStyleToString(uint8_t style) {
    for (const auto& entry : s_fillStyles) {
        if (entry.m_value == style) {
            return entry.m_name;
        }
    }
    return "UNKNOWN";
}

bool g_uiDatabaseReady;

// ref: FUN_1406031e0
void UiSetDatabaseReady() {
    g_uiDatabaseReady = true;
}
