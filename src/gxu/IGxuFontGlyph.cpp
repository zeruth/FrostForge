// Gxu glyph rendering (Engine/Source/Gxu/IGxuFontGlyph.cpp): a character's bitmap and metrics
// from FreeType, and the box drawn for characters a face lacks.

#include <cmath>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "base/CharClass.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_glyphFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\IGxuFontGlyph.cpp";

}

// A character rendered at a pixel size: its bitmap (gray, or one bit per pixel when
// monochrome), placed below `baseline`, its advance and bearing. A missing character that is
// white space gets an empty bitmap a quarter of the size wide; 0xfffd falls back to the box.
// ref: FUN_1406a0d30
bool IGxuFontGlyphRender(FT_FaceRec_* face, uint32_t pixelSize, uint32_t code, int32_t baseline,
    GXUGLYPHBITMAP* bitmap, bool monochrome, int32_t outline) {
    FT_UInt index = FT_Get_Char_Index(face, code);
    if (index != 0) {
        FT_Int32 loadFlags = FT_LOAD_NO_HINTING | FT_LOAD_PEDANTIC | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH
            | FT_LOAD_LINEAR_DESIGN | (monochrome ? 0 : FT_LOAD_NO_BITMAP);
        if (FT_Load_Glyph(face, index, loadFlags) == 0
            && FT_Render_Glyph(face->glyph, monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL) == 0) {
            FT_GlyphSlot slot = face->glyph;
            bool blank = false;
            uint32_t rows = slot->bitmap.rows;
            uint32_t pitch = static_cast<uint32_t>(slot->bitmap.pitch);
            uint32_t width = slot->bitmap.width;
            const uint8_t* source = slot->bitmap.buffer;
            uint32_t shownRows = rows;
            if (pixelSize < rows) {
                shownRows = pixelSize;
            }
            int32_t bytes = static_cast<int32_t>(rows * pitch);
            if (width == 0 || shownRows == 0 || source == nullptr || pitch == 0 || bytes == 0) {
                if (!CharIsSpace(code)) {
                    return false;
                }
                width = (pixelSize + 3) >> 2;
                if (width == 0) {
                    width = pixelSize;
                }
                pitch = width;
                if (monochrome) {
                    pitch = (width + 7) & 0xfffffff8;
                }
                blank = true;
                bytes = static_cast<int32_t>(pitch * pixelSize);
                shownRows = pixelSize;
            }
            auto data = static_cast<uint8_t*>(SMemAlloc(static_cast<uint64_t>(bytes), 0x1b, s_glyphFile,
                SStrHashFnv1a(s_glyphFile), 0xe2, 0));
            if (source == nullptr) {
                std::memset(data, 0, static_cast<size_t>(bytes));
            } else {
                std::memcpy(data, source, static_cast<size_t>(bytes));
            }
            bitmap->m_data = data;
            bitmap->m_width = width;
            bitmap->m_cellWidth = static_cast<uint32_t>(outline) + width;
            bitmap->m_rows = shownRows;
            bitmap->m_pitch = pitch;
            int32_t advance = slot->metrics.horiAdvance;
            bitmap->m_advance = static_cast<float>(advance / 64) + 1.0f;
            bitmap->m_bearing = static_cast<float>(slot->metrics.horiBearingX) / 64.0f;
            bitmap->m_yOffset = 0;
            bitmap->m_yOffsetBelow = 0;
            if (shownRows != 0 && width != 0 && data != nullptr && !blank) {
                int32_t above = 0;
                int32_t below = 0;
                if (shownRows <= pixelSize) {
                    int32_t top = slot->bitmap_top;
                    if (baseline < top) {
                        above = 0;
                        below = top - baseline;
                    } else {
                        above = baseline - top;
                        below = 0;
                    }
                }
                int32_t pad = 0;
                if (0 < above) {
                    pad = above;
                }
                int32_t room = static_cast<int32_t>(pixelSize - shownRows);
                if (room < pad) {
                    below = static_cast<int32_t>(pixelSize - pad) - static_cast<int32_t>(shownRows);
                }
                bitmap->m_yOffset = below;
                if (pad <= room) {
                    room = pad;
                }
                bitmap->m_yOffsetBelow = room;
            }
            return true;
        }
    }
    if (code != 0xfffd) {
        return false;
    }
    IGxuFontGlyphRenderMissing(pixelSize, bitmap, monochrome, outline);
    return true;
}

// The box for a missing character: a frame a fifth of the height thick around a lighter
// fill, three fifths of the size wide.
// ref: FUN_1406a0fb0
void IGxuFontGlyphRenderMissing(uint32_t pixelSize, GXUGLYPHBITMAP* bitmap, bool monochrome, int32_t outline) {
    uint32_t width = 2;
    if (0xe < pixelSize * 3) {
        width = (pixelSize * 3) / 5;
    }
    uint32_t border = width / 10;
    float level = fmodf(static_cast<float>(width) / 10.0f, 1.0f);
    uint32_t rows = pixelSize - border * 2;
    uint32_t fill = static_cast<uint32_t>(level * 255.0f);
    uint32_t pitch = width;
    if (monochrome) {
        pitch = (width + 7) >> 3;
    }
    uint32_t inner = width - border * 2;
    auto gray = static_cast<uint8_t*>(SMemAlloc(static_cast<uint64_t>(rows * width), 0x1b, s_glyphFile,
        SStrHashFnv1a(s_glyphFile), 0x60, 8));
    if (1 < rows) {
        uint8_t* cursor = gray;
        if (border * width != 0) {
            std::memset(cursor, 0xff, border * width);
            cursor += border * width;
        }
        if (border != 0) {
            std::memset(cursor, 0xff, border);
            cursor += border;
        }
        if (inner != 0) {
            std::memset(cursor, static_cast<int>(fill & 0xff), inner);
            cursor += inner;
        }
        if (border != 0) {
            std::memset(cursor, 0xff, border);
            cursor += border;
        }
        uint32_t middle = rows - border * 2 - 2;
        uint8_t edge = static_cast<uint8_t>(fill);
        for (uint32_t i = 0; i < middle; i++) {
            if (border == 0) {
                cursor[0] = edge;
                cursor[inner - 1] = edge;
                cursor += inner;
            } else {
                std::memset(cursor, 0xff, border);
                cursor += border;
                cursor[0] = edge;
                cursor[inner - 1] = edge;
                std::memset(cursor + inner, 0xff, border);
                cursor += inner + border;
            }
        }
        if (border != 0) {
            std::memset(cursor, 0xff, border);
            cursor += border;
        }
        if (inner != 0) {
            std::memset(cursor, static_cast<int>(fill & 0xff), inner);
            cursor += inner;
        }
        if (border != 0) {
            std::memset(cursor, 0xff, border);
            cursor += border;
        }
        if (border * width != 0) {
            std::memset(cursor, 0xff, border * width);
        }
    }
    uint8_t* data = gray;
    if (monochrome) {
        data = static_cast<uint8_t*>(SMemAlloc(static_cast<uint64_t>(pitch * rows), 0x1b, s_glyphFile,
            SStrHashFnv1a(s_glyphFile), 0x8e, 8));
        const uint8_t* source = gray;
        uint8_t* row = data;
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t bit = 7;
            uint8_t* out = row;
            for (uint32_t x = 0; x < width; x++) {
                *out |= static_cast<uint8_t>((0x7f < *source) << bit);
                source++;
                if (bit == 0) {
                    out++;
                }
                bit = (bit - 1) & 7;
            }
            row += pitch;
        }
        SMemFree(gray, s_glyphFile, 0x9b, 0);
    }
    bitmap->m_data = data;
    bitmap->m_width = width;
    bitmap->m_cellWidth = static_cast<uint32_t>(outline) + width;
    bitmap->m_rows = rows;
    bitmap->m_pitch = pitch;
    bitmap->m_yOffset = static_cast<int32_t>(border);
    bitmap->m_yOffsetBelow = static_cast<int32_t>(border);
    bitmap->m_advance = static_cast<float>(border * 3) + static_cast<float>(width) + 1.0f;
    bitmap->m_bearing = static_cast<float>(border) * 2.0f;
}
