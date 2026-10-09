// Gxu font layout (Engine/Source/Gxu/GxuFontUtil.cpp): how much of a string fits a width,
// where lines break, how wide a run of text is, and the |T / |A codes' parameters.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "base/CharClass.hpp"
#include "base/Coordinate.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"

namespace {

const char s_utilFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontUtil.cpp";

// The lengths of "|A" with its ':' and of "|T" (DAT_1426d29a4, DAT_1426d298c).
const int32_t ATLAS_PREFIX_LENGTH = 3;
const int32_t TEXTURE_PREFIX_LENGTH = 2;

// Round half away from zero, as the module converts screen fractions to pixels.
int32_t RoundToInt(float value) {
    if (value <= 0.0f) {
        return -static_cast<int32_t>(static_cast<int64_t>(0.5f - value));
    }
    return static_cast<int32_t>(static_cast<int64_t>(value + 0.5f));
}

// The extra width an outline adds to a line.
float OutlineWidth(CGxuFont* font, float indent) {
    float width = 0.0f;
    if (0.0f < indent) {
        width = ceilf(indent);
    }
    if ((font->m_flags & 8) == 0) {
        if ((font->m_flags & 1) != 0) {
            width += 2.0f;
        }
    } else {
        width += 4.0f;
    }
    return width;
}

// The step from one character to the next: kerned, or the monospaced cell.
float StepBetween(CGxuFont* font, uint32_t previous, uint32_t code, uint32_t flags) {
    if ((flags & 0x10) == 0) {
        return font->GetKernedAdvance(previous, code);
    }
    return font->GetMonoAdvance(previous, code);
}

// Copies one ':'-separated field (at most 31 characters) of a texture code.
void CopyField(char* out, const char* start, const char* end) {
    int64_t length = end - start;
    if (length + 1 >= 0x21) {
        length = 0x1f;
    }
    int64_t i = 0;
    for (; i < length && start[i] != '\0'; i++) {
        out[i] = start[i];
    }
    out[i] = '\0';
}

// A field's number, in hex when it starts 0x.
int32_t FieldInteger(const char* field) {
    int32_t base = 10;
    if (field[0] == '0' && (field[1] == 'x' || field[1] == 'X')) {
        base = 16;
    }
    return static_cast<int32_t>(strtol(field, nullptr, base));
}

// The end of the field starting at `start`: the next ':', or `limit`.
const char* FieldEnd(const char* start, const char* limit) {
    const char* colon = strchr(start, ':');
    if (colon == nullptr || limit < colon) {
        return limit;
    }
    return colon;
}

// How a code's size in points maps to the string's height in pixels.
float EmbeddedScale(float height) {
    float texels = g_coordinateSpace.m_aspectCompensation * 1024.0f;
    return (height / ((14.0f / texels) * g_coordinateSpace.m_diagonalWidth))
        * (static_cast<float>(g_gxuScreenHeight) / texels);
}

}

// The allocator FreeType is handed (FT_MemoryRec at 0x142cf5e10).
// ref: FUN_14068fcb0
void* GxuFontFreeTypeAlloc(void* memory, long size) {
    (void)memory;
    return SMemAlloc(static_cast<uint64_t>(size), 0x1b, s_utilFile, SStrHashFnv1a(s_utilFile), 0x270, 0);
}

// ref: FUN_14068fd10
void GxuFontFreeTypeFree(void* memory, void* block) {
    (void)memory;
    SMemFree(block, s_utilFile, 0x279, 0);
}

// ref: FUN_14068fd30
void* GxuFontFreeTypeRealloc(void* memory, long currentSize, long newSize, void* block) {
    (void)memory;
    (void)currentSize;
    return SMemReAlloc(block, static_cast<uint64_t>(newSize), 0x1b, s_utilFile, SStrHashFnv1a(s_utilFile), 0x286, 0);
}

// ref: FUN_140691810
float GxuFontPixelHeight(int32_t pixels) {
    return static_cast<float>(pixels) / static_cast<float>(g_gxuScreenHeight);
}

// A height in screen units as whole pixels, unless it is already in pixels.
// ref: FUN_140691830
float GxuFontRoundHeight(bool scaled, float height) {
    if (scaled) {
        return height;
    }
    return static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenHeight) * height));
}

// ref: FUN_140691890
float GxuFontRoundWidth(bool scaled, float width) {
    if (scaled) {
        return width;
    }
    return static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenWidth) * width));
}

// The height the font draws at one texel per pixel.
// ref: FUN_14068b180
float GxuFontGetOneToOneHeight(CGxuFont* font) {
    return static_cast<float>(font->GetPixelSize()) / static_cast<float>(g_gxuScreenHeight);
}

// A character's advance, kerned against the next printable character when there is one.
// ref: FUN_14068fda0
float GxuFontScaledAdvance(uint32_t code, CGxuFont* font, float height, uint32_t flags, const char* next) {
    if (code == 0) {
        return 0.0f;
    }
    float advance = 0.0f;
    if (*next != '\0') {
        uint32_t nextCode = 0;
        for (;;) {
            int32_t length = 0;
            int32_t kind = GxuDetermineControlCode(next, &length, nullptr, flags, &nextCode);
            next += length;
            if (kind == 0) {
                break;
            }
            if (kind == 3 || *next == '\0') {
                goto lookup;
            }
        }
        advance = StepBetween(font, code, nextCode, flags);
        if (advance != 0.0f) {
            return advance;
        }
    }
lookup:
    CharCodeDesc* glyph = font->GetGlyph(code);
    if (glyph != nullptr || (glyph = font->GetGlyph(0xfffd)) != nullptr) {
        advance = font->GetBearing(glyph, ((flags >> 7) & 1) != 0, height);
        advance += static_cast<float>(glyph->m_width);
    }
    return advance;
}

// Whether a line may break between two characters: never inside a number or before closing
// punctuation, always after a separator, and between CJK ideographs.
// ref: FUN_14068f500
bool GxuFontCanBreakBetween(uint32_t previous, uint32_t code) {
    if (previous == 0) {
        return false;
    }
    if (previous == 0x2d) {
        if (code - 0x2c < 2) {
            return false;
        }
        return true;
    }
    if (previous == 0x3b || previous == 0x2f || code == 0x7c || code == 0xffffffff) {
        return true;
    }
    if (CharIsSpace(previous)) {
        return false;
    }
    if (code != 0xa0 && CharIsSpace(code)) {
        return true;
    }
    bool opening;
    if (previous < 0x3011) {
        if (previous == 0x3010) {
            return false;
        }
        if (previous < 0x201d) {
            if (previous == 0x201c) {
                return false;
            }
            if (previous - 0x24 < 0x39 && ((0x180000000000011ull >> ((previous - 0x24) & 0x3f)) & 1) != 0) {
                return false;
            }
            if (previous == 0x7b) {
                return false;
            }
            opening = previous == 0x2018;
        } else {
            if (previous == 0x2035 || previous == 0x3008 || previous == 0x300a || previous == 0x300c) {
                return false;
            }
            opening = previous == 0x300e;
        }
    } else if (previous < 0xff09) {
        if (previous == 0xff08) {
            return false;
        }
        if (previous < 0xfe5c) {
            if (previous == 0xfe5b || previous == 0x3014 || previous == 0x301d) {
                return false;
            }
            opening = previous == 0xfe59;
        } else {
            if (previous == 0xfe5d) {
                return false;
            }
            opening = previous == 0xff04;
        }
    } else {
        if (previous == 0xff3b || previous == 0xff5b || previous == 0xffe1 || previous == 0xffe5) {
            return false;
        }
        opening = previous == 0xffe6;
    }
    if (opening) {
        return false;
    }
    if (code < 0x2014) {
        if (code == 0x2013) {
            return false;
        }
        switch (code) {
        case 0x21: case 0x25: case 0x29: case 0x2c: case 0x2e: case 0x3a: case 0x3b: case 0x3f: case 0x5d:
        case 0x7d: case 0xb0: case 0xb7:
            return false;
        }
    } else if (code < 0xfe31) {
        if (code == 0xfe30) {
            return false;
        }
        if (code < 0x3002) {
            if (code == 0x3001) {
                return false;
            }
            switch (code) {
            case 0x2014: case 0x2019: case 0x201d: case 0x2022: case 0x2026: case 0x2027: case 0x2032: case 0x2033:
            case 0x2103:
                return false;
            }
        } else {
            switch (code) {
            case 0x3002: case 0x3009: case 0x300b: case 0x300d: case 0x300f: case 0x3011: case 0x3015: case 0x301e:
            case 0x30fc:
                return false;
            }
        }
    } else if (code < 0xff5e) {
        if (code == 0xff5d) {
            return false;
        }
        switch (code) {
        case 0xfe50: case 0xfe51: case 0xfe52: case 0xfe54: case 0xfe55: case 0xfe56: case 0xfe57: case 0xfe5a:
        case 0xfe5c: case 0xfe5e: case 0xff01: case 0xff05: case 0xff09: case 0xff0c: case 0xff0e: case 0xff1a:
        case 0xff1b: case 0xff1f: case 0xff3d:
            return false;
        }
    } else if (code == 0xff70 || code == 0xff9e || code == 0xff9f || code == 0xffe0) {
        return false;
    }
    if (previous != 0x3002 && previous != 0xff0c && 0xff < code - 0x1100 && 0xa7af < code - 0x3000
        && 0x1ff < code - 0xf900 && 0x9f < code - 0xff00 && 0x3c < code - 0xffa0) {
        return false;
    }
    return true;
}

// The script a character belongs to: 0 Korean, 1 Chinese, 2 Cyrillic, 3 Latin, 4 other.
// ref: FUN_14068fc50
int32_t GxuFontScriptOf(uint32_t code) {
    if (CharIsKorean(code)) {
        return 0;
    }
    if (CharIsChinese(code)) {
        return 1;
    }
    if (CharIsCyrillic(code)) {
        return 2;
    }
    return 4 - (CharIsLatin(code) ? 1 : 0);
}

// The script a string needs, and how many of its characters the face lacks.
// ref: FUN_14068fb60
int32_t GxuFontMissingScript(const char* text, CGxuFont* font, int32_t* missing) {
    *missing = 0;
    int32_t script = 4;
    FT_FaceRec_* face = GxuFontFaceGetFace(font->m_face);
    if (text == nullptr) {
        return 4;
    }
    while (*text != '\0') {
        int32_t length = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(text), &length);
        text += length;
        if (0x1f < code && FT_Get_Char_Index(face, code) == 0) {
            if (script == 4) {
                script = GxuFontScriptOf(code & 0xffff);
            }
            *missing += 1;
        }
    }
    return script;
}

// One line of a string scaled to world units: how many bytes it takes up to a newline, its
// width, where the next line starts, and the tallest embedded texture.
// ref: FUN_14068eb90
void GxuFontGetScaledLineExtent(CGxuFont* font, const char* text, float height, int32_t* length, float* extent,
    const char** next, uint32_t flags, float* textureHeight, float scale) {
    if (height == 0.0f) {
        *length = 0;
        *extent = 0.0f;
        *next = nullptr;
        if (textureHeight != nullptr) {
            *textureHeight = 0.0f;
        }
        return;
    }
    uint32_t previous = 0;
    float width = 0.0f;
    float last = 0.0f;
    if (textureHeight != nullptr) {
        *textureHeight = 0.0f;
    }
    const char* start = text;
    while (*text != '\0') {
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(text, &advance, nullptr, flags, &code);
        if (kind == 3) {
            break;
        }
        switch (kind) {
        case 1: case 2: case 5: case 6: case 8: case 9: case 10: case 11: case 12:
            break;
        case 7:
        case 13: {
            GXUEMBEDDEDTEXTURE texture = {};
            bool parsed = kind == 13 ? GxuFontParseAtlasCode(text, &texture, 13.0f, scale, height)
                                     : GxuFontParseTextureCode(text, &texture, 7.0f, scale, height);
            if (parsed) {
                width += texture.m_height;
                if (textureHeight != nullptr && *textureHeight < texture.m_width) {
                    *textureHeight = texture.m_width;
                }
            }
            break;
        }
        default:
            if (font->GetGlyph(code) != nullptr) {
                if (previous == 0) {
                    last = 0.0f;
                } else {
                    last = StepBetween(font, previous, code, flags);
                }
                width += last;
                last = GxuFontScaledAdvance(code, font, 0.0f, 0, "");
                previous = code;
            }
            break;
        }
        text += advance;
    }
    *length = static_cast<int32_t>(text - start);
    *extent = (width + last) * (height / static_cast<float>(font->GetPixelSize()));
    while (*text != '\0') {
        int32_t advance = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(text), &advance);
        if (iswspace(static_cast<wint_t>(static_cast<int16_t>(code))) == 0) {
            break;
        }
        text += advance;
    }
    *next = text;
}

// One line of a string in screen units, broken to fit `width`: how many bytes it takes, its
// width, where the next line starts, whether it broke at a word, and the tallest texture.
// ref: FUN_14068eed0
void GxuFontGetWrappedLineExtent(CGxuFont* font, const char* text, float height, float width, int32_t* length,
    float* extent, const char** next, float indent, uint32_t flags, uint8_t* wrapped, float* textureHeight,
    float scale) {
    if (height == 0.0f || (flags & 4) != 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    float pen = OutlineWidth(font, indent);
    float pixelHeight = static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenHeight) * height));
    float limit = ceilf(static_cast<float>(g_gxuScreenWidth) * width
        * (static_cast<float>(font->GetPixelSize()) / pixelHeight));
    int32_t breakAt = 0;
    int32_t kind = 3;
    uint32_t previous = 0;
    float lastAdvance = 0.0f;
    float breakPen = 0.0f;
    float step = 0.0f;
    int32_t depth = 0;
    if (textureHeight != nullptr) {
        *textureHeight = 0.0f;
    }
    const char* cursor = text;
    while (*cursor != '\0') {
        int32_t advance = 0;
        uint32_t code = 0;
        kind = GxuDetermineControlCode(cursor, &advance, nullptr, flags, &code);
        if (kind == 3) {
            breakPen = lastAdvance + pen;
            cursor += advance;
            breakAt = static_cast<int32_t>(cursor - text);
            break;
        }
        int32_t nextDepth = depth;
        switch (kind) {
        case 1: case 2: case 5: case 6: case 8: case 9: case 10:
            break;
        case 7:
        case 13: {
            GXUEMBEDDEDTEXTURE texture = {};
            bool parsed = kind == 13 ? GxuFontParseAtlasCode(cursor, &texture, pixelHeight, scale, height)
                                     : GxuFontParseTextureCode(cursor, &texture, pixelHeight, scale, height);
            if (!parsed) {
                break;
            }
            if (depth < 1) {
                if (*wrapped != 0 || breakAt != 0) {
                    breakAt = static_cast<int32_t>(cursor - text);
                }
                *wrapped = 1;
                breakPen = lastAdvance + pen;
            }
            if (limit < lastAdvance + pen + texture.m_height) {
                goto done;
            }
            pen += lastAdvance;
            previous = 0x54;
            step = texture.m_height;
            lastAdvance = texture.m_height;
            if (textureHeight != nullptr && *textureHeight < texture.m_width) {
                *textureHeight = texture.m_width;
            }
            break;
        }
        case 11:
            nextDepth = depth + 1;
            break;
        case 12:
            nextDepth = depth < 1 ? depth : depth - 1;
            break;
        default: {
            CharCodeDesc* glyph = font->GetGlyph(code);
            if (glyph == nullptr) {
                code = 0xfffd;
                glyph = font->GetGlyph(0xfffd);
                if (glyph == nullptr) {
                    break;
                }
            }
            if (step == 0.0f && previous != 0) {
                step = StepBetween(font, previous, code, flags);
            }
            if (depth < 1 && GxuFontCanBreakBetween(previous, code)) {
                if (*wrapped != 0 || breakAt != 0) {
                    breakAt = static_cast<int32_t>(cursor - text);
                }
                *wrapped = 1;
                breakPen = lastAdvance + pen;
            }
            float glyphAdvance = GxuFontScaledAdvance(code, font, height, flags, cursor + advance);
            if (limit < step + pen + glyphAdvance) {
                goto done;
            }
            pen += step;
            step = 0.0f;
            lastAdvance = glyphAdvance;
            previous = code;
            break;
        }
        }
        depth = nextDepth;
        cursor += advance;
    }
done:
    float pixelsPerUnit = static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenHeight) * height))
        / static_cast<float>(font->GetPixelSize());
    float lineWidth = ((lastAdvance + pen) / static_cast<float>(g_gxuScreenWidth)) * pixelsPerUnit;
    if (kind == 3 || *cursor == '\0') {
        *extent = lineWidth;
        *next = cursor;
        *length = static_cast<int32_t>(cursor - text);
        return;
    }
    if (breakAt == 0 || (flags & 2) != 0) {
        *wrapped = static_cast<uint8_t>((flags >> 6) & 1);
        breakAt = static_cast<int32_t>(cursor - text);
    } else {
        cursor = text + breakAt;
        lineWidth = pixelsPerUnit * (breakPen / static_cast<float>(g_gxuScreenWidth));
    }
    *length = breakAt;
    *extent = lineWidth;
    while (*cursor != '\0') {
        int32_t advance = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(cursor), &advance);
        if (iswspace(static_cast<wint_t>(static_cast<int16_t>(code))) == 0) {
            break;
        }
        cursor += advance;
    }
    *next = cursor;
}

// ref: FUN_14068eac0
void GxuFontGetLineExtent(CGxuFont* font, const char* text, float height, float width, int32_t* length, float* extent,
    const char** next, float indent, uint32_t flags, uint8_t* wrapped, float* textureHeight, float scale) {
    if (0.0f <= height && 0.0f < width && text != nullptr && *text != '\0') {
        if (static_cast<int8_t>(flags) < 0) {
            GxuFontGetScaledLineExtent(font, text, height, length, extent, next, flags, textureHeight, scale);
            return;
        }
        GxuFontGetWrappedLineExtent(font, text, height, width, length, extent, next, indent, flags, wrapped,
            textureHeight, scale);
        return;
    }
    *length = 0;
    *extent = 0.0f;
    *next = nullptr;
}

// The width of up to `length` bytes of a line that fits `width`, how many characters it
// shows, and optionally each character's right edge and how many bytes were used.
// ref: FUN_14068ff00
int32_t GxuFontGetTextExtentWithPositions(CGxuFont* font, const char* text, float height, float width,
    uint32_t length, float* extent, uint32_t flags, float indent, float scale, int32_t* consumed, float* positions) {
    int32_t characters = 0;
    bool scaled = (flags & 0x80) != 0;
    if (!scaled && (height == 0.0f || (flags & 4) != 0)) {
        height = GxuFontGetOneToOneHeight(font);
    }
    float pen = OutlineWidth(font, indent);
    uint32_t previous = 0;
    float lastAdvance = 0.0f;
    float pixelHeight = height;
    if (!scaled) {
        pixelHeight = static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenHeight) * height));
    }
    float divisor = pixelHeight;
    if (divisor <= 1.0f) {
        divisor = 1.0f;
    }
    float limitPixels = (static_cast<float>(g_gxuScreenWidth) * width * static_cast<float>(font->GetPixelSize())) / divisor;
    uint32_t limit;
    if (limitPixels <= 0.0f) {
        limit = static_cast<uint32_t>(-static_cast<int32_t>(static_cast<int64_t>(-limitPixels)));
    } else {
        limit = static_cast<uint32_t>(static_cast<int64_t>(limitPixels + 0.99995f));
    }
    uint32_t pixelSize = font->GetPixelSize();
    const char* cursor = text;
    float* position = positions;
    while (*cursor != '\0' && length != 0) {
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(cursor, &advance, nullptr, flags, &code);
        if (length < static_cast<uint32_t>(advance)) {
            advance = static_cast<int32_t>(length);
        }
        length -= static_cast<uint32_t>(advance);
        cursor += advance;
        if (kind == 3) {
            break;
        }
        switch (kind) {
        case 1: case 2: case 5: case 6: case 8: case 9: case 10: case 11: case 12:
            break;
        case 7:
        case 13: {
            GXUEMBEDDEDTEXTURE texture = {};
            bool parsed = kind == 13 ? GxuFontParseAtlasCode(cursor - advance, &texture, pixelHeight, scale, height)
                                     : GxuFontParseTextureCode(cursor - advance, &texture, pixelHeight, scale, height);
            if (parsed) {
                if (static_cast<float>(limit) < texture.m_height + pen) {
                    cursor -= advance;
                    goto done;
                }
                previous = 0;
                pen += texture.m_height;
                characters++;
            }
            break;
        }
        default: {
            CharCodeDesc* glyph = font->GetGlyph(code);
            if (glyph == nullptr) {
                code = 0xfffd;
                glyph = font->GetGlyph(0xfffd);
                if (glyph == nullptr) {
                    characters++;
                    break;
                }
            }
            float step = 0.0f;
            if (previous != 0) {
                step = StepBetween(font, previous, code, flags);
            }
            float glyphAdvance = GxuFontScaledAdvance(code, font, height, flags, cursor);
            if (static_cast<float>(limit) < step + pen + glyphAdvance) {
                cursor -= advance;
                goto done;
            }
            pen += step;
            characters++;
            lastAdvance = glyphAdvance;
            previous = code;
            if (positions != nullptr) {
                float right = (pixelHeight / static_cast<float>(pixelSize)) * pen;
                if (!scaled) {
                    right /= static_cast<float>(g_gxuScreenWidth);
                }
                *position++ = right;
            }
            break;
        }
        }
    }
done:
    float total = (pen + lastAdvance) * (pixelHeight / static_cast<float>(pixelSize));
    if (!scaled) {
        total /= static_cast<float>(g_gxuScreenWidth);
    }
    *extent = total;
    if (consumed != nullptr) {
        *consumed = static_cast<int32_t>(cursor - text);
    }
    return characters;
}

// The width of the widest line of up to `length` bytes, breaking only at newlines.
// ref: FUN_1406903a0
void GxuFontGetWrappedExtent(CGxuFont* font, const char* text, uint32_t length, float height, float* extent,
    uint32_t flags, float indent, float scale) {
    (void)scale;
    if (height == 0.0f || (flags & 4) != 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    float pen = OutlineWidth(font, indent);
    uint32_t previous = 0;
    float lastAdvance = 0.0f;
    float widest = 0.0f;
    while (*text != '\0' && length != 0) {
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(text, &advance, nullptr, flags, &code);
        if (static_cast<int32_t>(length) < advance) {
            advance = static_cast<int32_t>(length);
        }
        advance = static_cast<int32_t>(strnlen(text, static_cast<size_t>(advance)));
        length -= static_cast<uint32_t>(advance);
        switch (kind) {
        case 1: case 2: case 5: case 6: case 8: case 9: case 10: case 11: case 12:
            break;
        case 3:
        newline:
            if (widest < lastAdvance + pen) {
                widest = lastAdvance + pen;
            }
            pen = 0.0f;
            lastAdvance = 0.0f;
            break;
        case 7:
        case 13: {
            GXUEMBEDDEDTEXTURE texture = {};
            bool parsed = kind == 13 ? GxuFontParseAtlasCode(text, &texture, 13.0f, scale, height)
                                     : GxuFontParseTextureCode(text, &texture, 7.0f, scale, height);
            if (parsed) {
                pen += texture.m_height;
            }
            break;
        }
        default: {
            if (code == 10) {
                goto newline;
            }
            CharCodeDesc* glyph = font->GetGlyph(code);
            if (glyph == nullptr) {
                code = 0xfffd;
                if (font->GetGlyph(0xfffd) == nullptr) {
                    break;
                }
            }
            float step = 0.0f;
            if (previous != 0) {
                step = StepBetween(font, previous, code, flags);
            }
            lastAdvance = GxuFontScaledAdvance(code, font, 0.0f, 0, "");
            pen += step;
            previous = code;
            break;
        }
        }
        text += advance;
    }
    if (widest < lastAdvance + pen) {
        widest = lastAdvance + pen;
    }
    if (static_cast<int8_t>(flags) < 0) {
        *extent = (height / static_cast<float>(font->GetPixelSize())) * widest;
        return;
    }
    float pixelHeight = static_cast<float>(RoundToInt(static_cast<float>(g_gxuScreenHeight) * height));
    *extent = ((pixelHeight / static_cast<float>(font->GetPixelSize())) * widest) / static_cast<float>(g_gxuScreenWidth);
}

// |A<atlas>:<height>:<width>:<x>:<y>:<r>:<g>:<b>|a: an atlas image, sized from the line.
// ref: FUN_1406907c0
bool GxuFontParseAtlasCode(const char* text, GXUEMBEDDEDTEXTURE* texture, float lineHeight, float scale,
    float height) {
    (void)scale;
    float sizeScale = EmbeddedScale(height);
    if (text == nullptr || *text == '\0' || texture == nullptr) {
        return false;
    }
    std::memset(reinterpret_cast<uint8_t*>(texture) + 0x10, 0, 0x46);
    text += ATLAS_PREFIX_LENGTH;
    const char* end = strchr(text, '|');
    if (end == nullptr || end <= text || end[1] != 'a') {
        return false;
    }
    const char* colon = strchr(text, ':');
    if (colon == nullptr || end <= colon) {
        return false;
    }
    int32_t id = atoi(text);
    if (id == 0) {
        texture->m_atlas = text;
        texture->m_atlasLength = static_cast<int32_t>(colon - text);
    } else {
        texture->m_atlasId = id;
    }
    char field[80];
    const char* start = colon + 1;
    const char* fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return false;
    }
    CopyField(field, start, fieldEnd);
    float value = SStrToFloat(field) * sizeScale;
    texture->m_height = value;
    if (0.0f >= value) {
        texture->m_height = lineHeight;
    }
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    float second = SStrToFloat(field);
    if (0.0f < second) {
        lineHeight = 0.0f < value ? second * sizeScale : second * texture->m_height;
    }
    texture->m_width = lineHeight;
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_offsetX = SStrToFloat(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_offsetY = SStrToFloat(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[3] = 0xff;
    texture->m_vertexColor[2] = static_cast<uint8_t>(FieldInteger(field));
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[1] = static_cast<uint8_t>(FieldInteger(field));
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[0] = static_cast<uint8_t>(FieldInteger(field));
    texture->m_hasVertexColor = 1;
    return true;
}

// |T<path>:<height>:<width>:<x>:<y>:<texW>:<texH>:<l>:<r>:<t>:<b>:<r>:<g>:<b>|t: a texture.
// ref: FUN_140690da0
bool GxuFontParseTextureCode(const char* text, GXUEMBEDDEDTEXTURE* texture, float lineHeight, float scale,
    float height) {
    (void)scale;
    float sizeScale = EmbeddedScale(height);
    if (text == nullptr || *text == '\0' || texture == nullptr) {
        return false;
    }
    std::memset(reinterpret_cast<uint8_t*>(texture) + 0x10, 0, 0x46);
    text += TEXTURE_PREFIX_LENGTH;
    const char* end = strchr(text, '|');
    if (end == nullptr || end <= text || end[1] != 't') {
        return false;
    }
    const char* colon = strchr(text, ':');
    if (colon == nullptr || end <= colon) {
        return false;
    }
    texture->m_path = text;
    texture->m_pathLength = static_cast<int32_t>(colon - text);
    texture->m_fileDataId = atoi(text);
    if (texture->m_fileDataId != 0) {
        texture->m_path = nullptr;
    }
    char field[80];
    const char* start = colon + 1;
    const char* fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return false;
    }
    CopyField(field, start, fieldEnd);
    float value = SStrToFloat(field) * sizeScale;
    texture->m_height = value;
    if (0.0f >= value) {
        texture->m_height = lineHeight;
    }
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        texture->m_width = texture->m_height;
        return true;
    }
    CopyField(field, start, fieldEnd);
    float second = SStrToFloat(field);
    if (0.0f < second) {
        lineHeight = 0.0f < value ? second * sizeScale : second * texture->m_height;
    }
    texture->m_width = lineHeight;
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_offsetX = SStrToFloat(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_offsetY = SStrToFloat(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    int32_t textureWidth = FieldInteger(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    int32_t textureHeight = FieldInteger(field);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (textureHeight <= 0 || textureWidth <= 0 || fieldEnd == nullptr || end < fieldEnd || end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_texCoords[1] = static_cast<float>(FieldInteger(field)) / static_cast<float>(textureWidth);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_texCoords[3] = static_cast<float>(FieldInteger(field)) / static_cast<float>(textureWidth);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_texCoords[0] = static_cast<float>(FieldInteger(field)) / static_cast<float>(textureHeight);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_hasTexCoords = 1;
    texture->m_texCoords[2] = static_cast<float>(FieldInteger(field)) / static_cast<float>(textureHeight);
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[3] = 0xff;
    texture->m_vertexColor[2] = static_cast<uint8_t>(FieldInteger(field));
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[1] = static_cast<uint8_t>(FieldInteger(field));
    start = fieldEnd + 1;
    fieldEnd = FieldEnd(start, end);
    if (end <= start) {
        return true;
    }
    CopyField(field, start, fieldEnd);
    texture->m_vertexColor[0] = static_cast<uint8_t>(FieldInteger(field));
    texture->m_hasVertexColor = 1;
    return true;
}
