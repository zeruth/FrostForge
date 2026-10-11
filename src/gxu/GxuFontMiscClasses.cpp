// Gxu glyph cache (Engine/Source/Gxu/GxuFontMiscClasses.cpp): glyphs converted into cache
// cells, the rows of a glyph page they are packed into, and the page textures.

#include <cstring>
#include <new>

#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_miscFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontMiscClasses.cpp";
const char s_glyphListTag[] = ".?AV?$TSList@UCharCodeDesc@@V?$TSGetExplicitLink@UCharCodeDesc@@@@$07@@";
const char s_glyphTableTag[] = ".?AV?$TSHashTable@UCharCodeDesc@@VHASHKEY_NONE@@$07@@";
const char s_kernListTag[] = ".?AV?$TSList@UKernNode@@V?$TSGetExplicitLink@UKernNode@@@@$07@@";
const char s_kernTableTag[] = ".?AV?$TSHashTable@UKernNode@@VKERNINGHASHKEY@@$07@@";
const char s_rowArrayTag[] = ".?AV?$TSFixedArray@UTEXTURECACHEROW@@$07@@";

// The outline passes' grids, 256 cells wide (the statics at 0x142e9cfe8, 0x142e9d008 and
// 0x142e9d028, and the thread's buffer for the monochrome pass).
uint16_t s_outlineMask[0x10000];
uint16_t s_outlineGray[0x10000];
uint16_t s_outlineAlpha[0x10000];
thread_local uint16_t t_monoGrid[0x10000];

// An outline cell's alpha from how many of its 3x3 neighbourhood are covered (DAT_1426d2d28).
const uint8_t s_coverageAlpha[10] = { 0, 1, 1, 3, 5, 7, 9, 11, 13, 15 };

// Every page texture is assembled here before it is uploaded (DAT_142e9d050).
uint8_t s_pageStaging[0x400 * 0x400 * 2];

// The node before `glyph` in its row, or null at the front.
CharCodeDesc* RowPrevious(CharCodeDesc* glyph) {
    return glyph->m_rowLink.m_prevlink->m_prevlink->Next();
}

// Whether any cell in the clipped 8-neighbourhood of (x, y) has a bit of `mask`.
bool AnyNeighbour(const uint16_t* grid, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint16_t mask) {
    for (int32_t dy = -1; dy <= 1; dy++) {
        if ((y == 0 && dy < 0) || (y == height - 1 && dy > 0)) {
            continue;
        }
        for (int32_t dx = -1; dx <= 1; dx++) {
            if ((dx == 0 && dy == 0) || (x == 0 && dx < 0) || (x == width - 1 && dx > 0)) {
                continue;
            }
            if ((grid[(y + dy) * 0x100 + x + dx] & mask) != 0) {
                return true;
            }
        }
    }
    return false;
}

// How many covered cells the clipped 3x3 around (x, y) holds, itself included.
uint32_t CoveredAround(const uint16_t* grid, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t count = 0;
    for (int32_t dy = -1; dy <= 1; dy++) {
        if ((y == 0 && dy < 0) || (y == height - 1 && dy > 0)) {
            continue;
        }
        for (int32_t dx = -1; dx <= 1; dx++) {
            if ((x == 0 && dx < 0) || (x == width - 1 && dx > 0)) {
                continue;
            }
            if (grid[(y + dy) * 0x100 + x + dx] != 0) {
                count++;
            }
        }
    }
    return count;
}

// The largest free run in a row: before its first glyph, between glyphs, and after its last.
uint32_t RowLargestGap(TEXTURECACHEROW* row, uint32_t length) {
    CharCodeDesc* first = row->m_glyphs.Head();
    if (first == nullptr) {
        return 0;
    }
    CharCodeDesc* previous = RowPrevious(first);
    uint32_t largest = previous != nullptr ? first->m_top - previous->m_bottom - 1 : first->m_top;
    for (CharCodeDesc* glyph = first; glyph != nullptr; glyph = glyph->m_rowLink.Next()) {
        CharCodeDesc* next = glyph->m_rowLink.Next();
        uint32_t gap = next != nullptr ? next->m_top - glyph->m_bottom - 1 : length - glyph->m_bottom - 2;
        if (largest < gap) {
            largest = gap;
        }
    }
    return largest;
}

// Where a glyph sits: its strip across the page and its run along it.
void PlaceGlyph(CharCodeDesc* glyph, uint32_t top, uint32_t rowIndex, uint32_t cellSize, uint32_t rowsPerLayer) {
    glyph->m_top = top;
    glyph->m_bottom = top + glyph->m_width - 1;
    glyph->m_row = rowIndex;
    uint32_t x = (rowIndex % rowsPerLayer) * cellSize;
    glyph->m_texCoords[0] = static_cast<float>(x);
    glyph->m_texCoords[1] = static_cast<float>(top);
    glyph->m_texCoords[2] = static_cast<float>(x + cellSize);
    glyph->m_texCoords[3] = static_cast<float>(glyph->m_width) + static_cast<float>(top);
}

// A glyph out of its row, its font's glyph table and its use list, freed.
void FreeGlyph(CharCodeDesc* glyph) {
    glyph->m_recentLink.Unlink();
    glyph->m_rowLink.Unlink();
    if (glyph->m_table != nullptr && glyph->m_linktoslot.IsLinked()) {
        static_cast<CHARCODETABLE*>(glyph->m_table)->Unlink(glyph);
    }
    glyph->m_linktofull.Unlink();
    glyph->m_linktoslot.Unlink();
    SMemFree(glyph, s_glyphListTag, -2, 0);
}

// Copies one glyph's cell into the staging page at its layer (FUN_1406a0740).
// ref: FUN_1406a0740
void CopyGlyphToPage(TEXTURECACHE* cache, CharCodeDesc* glyph, uint8_t* dest, uint32_t width, uint32_t layer) {
    const uint8_t* source = glyph->Bitmap();
    uint32_t rows = cache->m_font->m_cellSize;
    if ((cache->m_font->m_flags & 1) == 0) {
        uint8_t keep;
        uint32_t shift;
        if ((layer < 2 && (dest += 1, layer == 0)) || layer == 2) {
            keep = 0x0f;
            shift = 4;
        } else {
            keep = 0xf0;
            shift = 0;
        }
        uint32_t nibble = 0;
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* out = dest;
            for (uint32_t x = 0; x < glyph->m_width; x++) {
                uint8_t packed = *source;
                source += nibble;
                *out = static_cast<uint8_t>((((packed >> (((1 - nibble) & 7) << 2)) & 0xf) << shift) | (*out & keep));
                nibble ^= 1;
                out += 2;
            }
            dest += static_cast<size_t>(width) * 2;
        }
        return;
    }
    uint8_t* out = layer != 0 ? dest : dest + 1;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t* cell = out;
        for (uint32_t x = 0; x < glyph->m_width; x++) {
            *cell = *source++;
            cell += 2;
        }
        out += static_cast<size_t>(width) * 2;
    }
}

// Fills the staging page from every glyph a page holds and hands it to the upload (the
// texture's update callback, LAB_1406a03e0 into FUN_1406a0b80).
// ref: FUN_1406a0b80
void FillPage(void* param, uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)height;
    (void)slice;
    (void)mip;
    auto cache = *static_cast<TEXTURECACHE**>(param);
    uint32_t rowCount = cache->m_rowCount;
    uint32_t rowsPerLayer = rowCount / cache->m_layers;
    for (uint32_t index = rowCount; index != 0;) {
        index--;
        TEXTURECACHEROW* row = &cache->m_rows[index];
        for (CharCodeDesc* glyph = row->m_glyphs.Head(); glyph != nullptr; glyph = glyph->m_rowLink.Next()) {
            if (cache->m_texture != nullptr && cache->m_font != nullptr && cache->m_font->m_cellSize != 0) {
                uint32_t cell = cache->m_font->m_cellSize;
                uint8_t* dest = s_pageStaging
                    + (static_cast<size_t>((glyph->m_row % rowsPerLayer) * cell * cache->m_width) + glyph->m_top) * 2;
                CopyGlyphToPage(cache, glyph, dest, cache->m_width, index / (rowCount / cache->m_layers));
            }
        }
    }
    *pitch = width * 2;
    *data = s_pageStaging;
}

// A gray glyph with its outline: the glyph's cells, a ring around them (two with a thick
// outline), and each cell's alpha from its neighbourhood; one byte per cell.
// ref: FUN_14069ac90
void ConvertOutline(CharCodeDesc* glyph, GXUGLYPHBITMAP* bitmap, CGxuFont* font) {
    const uint8_t* source = bitmap->m_data;
    bool thick = (font->m_flags & 8) != 0;
    std::memset(s_outlineMask, 0, sizeof(s_outlineMask));
    std::memset(s_outlineGray, 0, sizeof(s_outlineGray));
    uint32_t start = (thick ? 1 : 0) + 0x101 + static_cast<uint32_t>(bitmap->m_yOffsetBelow) * 0x100;
    for (uint32_t y = 0; y < bitmap->m_rows; y++) {
        for (uint32_t x = 0; x < bitmap->m_width; x++) {
            if (source[x] != 0) {
                s_outlineMask[start + x] = 1;
                s_outlineGray[start + x] = source[x];
            }
        }
        start += 0x100;
        source += bitmap->m_pitch;
    }
    uint32_t height = font->m_cellSize;
    uint32_t width = bitmap->m_cellWidth;
    uint32_t passes = thick ? 2 : 1;
    for (uint32_t pass = 0; pass < passes; pass++) {
        uint16_t mark = pass != 0 ? 4 : 2;
        uint16_t test = pass != 0 ? 3 : 1;
        int32_t first = bitmap->m_yOffsetBelow - 1;
        if (first < 0) {
            first = 0;
        }
        for (uint32_t y = static_cast<uint32_t>(first); y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t index = y * 0x100 + x;
                if ((s_outlineMask[index] & test) == 0 && AnyNeighbour(s_outlineMask, x, y, width, height, test)) {
                    s_outlineMask[index] = mark;
                }
            }
        }
    }
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            s_outlineAlpha[y * 0x100 + x] = s_coverageAlpha[CoveredAround(s_outlineMask, x, y, width, height)];
        }
    }
    uint8_t* out = glyph->Bitmap();
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t index = y * 0x100 + x;
            uint8_t value = 0;
            if (s_outlineMask[index] != 0) {
                uint8_t gray = static_cast<uint8_t>(s_outlineGray[index]);
                value = static_cast<uint8_t>(s_outlineAlpha[index] << 4);
                if (gray != 0) {
                    value |= static_cast<uint8_t>((static_cast<int32_t>(gray) * 0xff) >> 0xc);
                }
            }
            *out++ = value;
        }
    }
}

// A monochrome glyph with its outline: 0xff for the glyph, 0xf0 for the first ring and 0x70
// for the second.
// ref: FUN_14069b6b0
void ConvertMonoOutline(CharCodeDesc* glyph, GXUGLYPHBITMAP* bitmap, CGxuFont* font) {
    const uint8_t* source = bitmap->m_data;
    bool thick = (font->m_flags & 8) != 0;
    uint16_t* grid = t_monoGrid;
    std::memset(grid, 0, sizeof(t_monoGrid));
    if (bitmap->m_rows != 0) {
        uint16_t* row = grid + (thick ? 2 : 1) + (static_cast<uint32_t>(bitmap->m_yOffsetBelow) + 1) * 0x100;
        for (uint32_t y = 0; y < bitmap->m_rows; y++) {
            for (uint32_t x = 0; x < bitmap->m_width; x++) {
                row[x] = static_cast<uint16_t>((static_cast<int8_t>(source[x >> 3]) >> (7 - (x & 7))) & 1);
            }
            row += 0x100;
            source += bitmap->m_pitch;
        }
    }
    uint32_t height = font->m_cellSize;
    uint32_t width = bitmap->m_cellWidth;
    uint32_t passes = thick ? 2 : 1;
    for (uint32_t pass = 0; pass < passes; pass++) {
        uint32_t first = static_cast<uint32_t>(bitmap->m_yOffsetBelow);
        if (pass == 1 && first != 0) {
            first--;
        }
        for (uint32_t y = first; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t index = y * 0x100 + x;
                uint16_t test;
                uint16_t mark;
                if (pass == 1) {
                    if (grid[index] != 0) {
                        continue;
                    }
                    mark = 2;
                    test = 4;
                } else {
                    test = 1;
                    mark = 4;
                }
                if (AnyNeighbour(grid, x, y, width, height, test) && grid[index] != 1) {
                    grid[index] = mark;
                }
            }
        }
    }
    uint8_t* out = glyph->Bitmap();
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t cell = grid[y * 0x100 + x];
            *out++ = cell == 1 ? 0xff : cell == 2 ? 0x70 : cell == 4 ? 0xf0 : 0;
        }
    }
}

// A rendered glyph into its cell: four bits per pixel packed from the row its padding
// starts on, or an outline's one byte per pixel.
// ref: FUN_14069aaf0
void ConvertGlyph(CharCodeDesc* glyph, GXUGLYPHBITMAP* bitmap, CGxuFont* font) {
    bool monochrome = (font->m_flags & 2) != 0;
    if ((font->m_flags & 1) != 0) {
        if (!monochrome) {
            ConvertOutline(glyph, bitmap, font);
        } else {
            ConvertMonoOutline(glyph, bitmap, font);
        }
        return;
    }
    const uint8_t* source = bitmap->m_data;
    uint8_t* cells = glyph->Bitmap();
    std::memset(cells, 0, (font->m_cellSize * bitmap->m_cellWidth + 1) >> 1);
    uint32_t start = bitmap->m_width * static_cast<uint32_t>(bitmap->m_yOffsetBelow);
    uint32_t nibble = start & 1;
    uint8_t* out = cells + (start >> 1);
    for (uint32_t y = 0; y < bitmap->m_rows; y++) {
        for (uint32_t x = 0; x < bitmap->m_width; x++) {
            uint32_t shift = nibble * 4;
            uint8_t value;
            if (!monochrome) {
                value = static_cast<uint8_t>((source[x] & 0xf0) >> shift);
            } else {
                value = static_cast<uint8_t>((((static_cast<int8_t>(source[x >> 3]) >> (7 - (x & 7))) & 1) * 0xf)
                    << ((4 - shift) & 0x1f));
            }
            *out = static_cast<uint8_t>(value | ((0x0f << shift) & *out));
            out += nibble;
            nibble ^= 1;
        }
        source += bitmap->m_pitch;
    }
}

}

// A glyph's cache cell, made from its rendering.
// ref: FUN_14069bcb0
CharCodeDesc* GxuFontNewGlyph(GXUGLYPHBITMAP* bitmap, CGxuFont* font, uint32_t code) {
    uint32_t size = font->m_cellSize * bitmap->m_cellWidth;
    if ((font->m_flags & 1) == 0) {
        size = (size + 1) >> 1;
    }
    void* memory = SMemAlloc(size + sizeof(CharCodeDesc), 0x1b, s_miscFile, SStrHashFnv1a(s_miscFile), 0x2e, 0);
    auto glyph = new (memory) CharCodeDesc();
    std::memset(glyph->m_texCoords, 0, sizeof(glyph->m_texCoords));
    glyph->m_code = 0;
    glyph->m_page = 0xffffffff;
    glyph->m_row = 0xffffffff;
    glyph->m_top = 0xffffffff;
    glyph->m_bottom = 0;
    glyph->m_advance = bitmap->m_advance;
    glyph->m_bearing = bitmap->m_bearing;
    glyph->m_width = bitmap->m_cellWidth;
    glyph->m_yOffset = bitmap->m_yOffset;
    glyph->m_glyphCode = code;
    glyph->m_pad84 = 0;
    ConvertGlyph(glyph, bitmap, font);
    return glyph;
}

// A cell for a glyph in a row of a page, in the first gap it fits (before the first glyph
// when it fits there, against it).
// ref: FUN_14069bdd0
CharCodeDesc* TextureCacheRowAllocate(TEXTURECACHEROW* row, uint32_t code, GXUGLYPHBITMAP* bitmap, uint32_t rowIndex,
    uint32_t cellSize, uint32_t length, uint32_t rowsPerLayer, CGxuFont* font) {
    uint32_t need = bitmap->m_cellWidth;
    if (row->m_largestGap < need) {
        return nullptr;
    }
    CharCodeDesc* first = row->m_glyphs.Head();
    if (first == nullptr) {
        CharCodeDesc* glyph = GxuFontNewGlyph(bitmap, font, code);
        row->m_glyphs.LinkToTail(glyph);
        row->m_largestGap -= need;
        PlaceGlyph(glyph, 0, rowIndex, cellSize, rowsPerLayer);
        return glyph;
    }
    CharCodeDesc* previous = RowPrevious(first);
    uint32_t gap = previous != nullptr ? first->m_top - previous->m_bottom - 1 : first->m_top;
    row->m_largestGap = gap;
    if (need <= gap) {
        CharCodeDesc* glyph = GxuFontNewGlyph(bitmap, font, code);
        TSLink<CharCodeDesc>* before = &first->m_rowLink;
        TSLink<CharCodeDesc>* link = &glyph->m_rowLink;
        link->m_prevlink = before->m_prevlink;
        link->m_next = first;
        before->m_prevlink->m_next = glyph;
        before->m_prevlink = link;
        uint32_t bottom = first->m_top - 1;
        PlaceGlyph(glyph, bottom - need + 1, rowIndex, cellSize, rowsPerLayer);
        row->m_largestGap = RowLargestGap(row, length);
        return glyph;
    }
    CharCodeDesc* placed = nullptr;
    for (CharCodeDesc* glyph = first; glyph != nullptr;) {
        CharCodeDesc* next = glyph->m_rowLink.Next();
        uint32_t after = next != nullptr ? next->m_top - glyph->m_bottom - 1 : length - glyph->m_bottom - 2;
        if (placed == nullptr) {
            if (need <= after) {
                CharCodeDesc* fresh = GxuFontNewGlyph(bitmap, font, code);
                TSLink<CharCodeDesc>* link = &fresh->m_rowLink;
                link->m_prevlink = &glyph->m_rowLink;
                link->m_next = glyph->m_rowLink.m_next;
                glyph->m_rowLink.NextLink(row->m_glyphs.m_linkoffset)->m_prevlink = link;
                glyph->m_rowLink.m_next = fresh;
                PlaceGlyph(fresh, glyph->m_bottom + 1, rowIndex, cellSize, rowsPerLayer);
                placed = fresh;
                next = fresh;
            }
        } else if (row->m_largestGap < after) {
            row->m_largestGap = after;
        }
        glyph = next;
    }
    return placed;
}

// Frees glyphs from `victim` onward (and back from it when that is not enough) until a
// glyph of the new bitmap's width fits, remembering their codes.
// ref: FUN_14069c330
void TextureCacheRowEvict(TEXTURECACHEROW* row, CharCodeDesc* victim, GXUGLYPHBITMAP* bitmap, uint32_t length,
    CGxuFont* font) {
    uint32_t need = bitmap->m_cellWidth;
    CharCodeDesc* previous = RowPrevious(victim);
    uint32_t freed = 0;
    for (CharCodeDesc* glyph = victim; glyph != nullptr && freed < need;) {
        CharCodeDesc* next = glyph->m_rowLink.Next();
        uint32_t span = glyph->m_bottom - glyph->m_top;
        if (next != nullptr) {
            freed += (next->m_top - glyph->m_bottom) + span;
        } else {
            freed += (span - glyph->m_bottom) - 1 + length;
        }
        font->NoteEvicted(glyph->m_glyphCode);
        FreeGlyph(glyph);
        glyph = next;
    }
    while (previous != nullptr && freed < need) {
        CharCodeDesc* before = RowPrevious(previous);
        if (before != nullptr) {
            freed += previous->m_bottom - before->m_bottom;
        } else {
            freed += 1 + previous->m_bottom;
        }
        font->NoteEvicted(previous->m_glyphCode);
        FreeGlyph(previous);
        previous = before;
    }
    row->m_largestGap = RowLargestGap(row, length);
}

// A page's texture, sized for the font's cell (FUN_1406a0570).
// ref: FUN_1406a0570
void TextureCacheCreate(TEXTURECACHE* cache, bool scalable, uint32_t cellSize) {
    uint64_t flags = (scalable ? 1 : 0) | 0x82000;
    uint32_t width;
    uint32_t height;
    if (cellSize < 0x21) {
        width = 0x100;
        height = 0x80;
    } else if (cellSize < 0x41) {
        width = 0x100;
        height = 0x100;
    } else if (cellSize < 0x56) {
        width = 0x200;
        height = 0x100;
    } else if (cellSize <= 0x80) {
        width = 0x200;
        height = 0x200;
    } else {
        width = 0x400;
        height = cellSize < 0xab ? 0x200 : 0x400;
    }
    cache->m_width = width;
    cache->m_height = height < 0x400 ? height : 0x400;
    // The texture description (FUN_14228c330).
    struct {
        uint8_t m_kind;
        uint8_t m_pad01[3];
        uint32_t m_width;
        uint32_t m_height;
        uint32_t m_depth;
        uint8_t m_format;
        uint8_t m_mipMode;
        uint8_t m_pad12[2];
        uint64_t m_flags;
        uint64_t m_pad1c;
        void* m_data;
        void* m_name;
    } desc = {};
    std::memset(&desc, 0, sizeof(desc));
    desc.m_width = cache->m_width;
    desc.m_height = cache->m_height;
    desc.m_depth = 1;
    desc.m_format = 3;
    desc.m_mipMode = 3;
    std::memcpy(reinterpret_cast<uint8_t*>(&desc) + 0x14, &flags, sizeof(flags));
    void* texture = nullptr;
    GxTexCreate(&desc, &texture);
    void* old = cache->m_texture;
    cache->m_texture = texture;
    if (old != nullptr) {
        GxTexDestroy(&old);
    }
}

// TSFixedArray<TEXTURECACHEROW, 8>'s clear (FUN_1406a04a0).
// ref: FUN_1406a04a0
void TextureCacheFreeRows(TEXTURECACHE* cache) {
    for (uint32_t i = 0; i < cache->m_rowCount; i++) {
        cache->m_rows[i].m_glyphs.UnlinkAll();
        cache->m_rows[i].m_glyphs.m_terminator.Unlink();
    }
    if (cache->m_rows != nullptr) {
        SMemFree(cache->m_rows, s_rowArrayTag, -2, 0);
    }
    cache->m_rowCapacity = 0;
    cache->m_rowCount = 0;
    cache->m_rows = nullptr;
}

// The page's rows: one per cell across the texture per layer, each as long as the texture.
// ref: FUN_1406a0660
void TextureCacheSetRows(TEXTURECACHE* cache, CGxuFont* font, uint32_t cellSize, uint32_t layers) {
    cache->m_font = font;
    cache->m_layers = layers;
    uint32_t count = (cache->m_height / cellSize) * layers;
    if (count != cache->m_rowCount) {
        if (count == 0) {
            TextureCacheFreeRows(cache);
            return;
        }
        // TSFixedArray<TEXTURECACHEROW, 8>::ReallocData (FUN_1406a0880).
        TEXTURECACHEROW* old = cache->m_rows;
        for (uint32_t i = count; i < cache->m_rowCount; i++) {
            old[i].m_glyphs.UnlinkAll();
            old[i].m_glyphs.m_terminator.Unlink();
        }
        cache->m_rowCapacity = count;
        cache->m_rows = static_cast<TEXTURECACHEROW*>(SMemAlloc(static_cast<uint64_t>(count) << 5, 8, s_rowArrayTag, 0, -7, 0));
        uint32_t keep = 0;
        if (old != nullptr) {
            keep = cache->m_rowCount < count ? cache->m_rowCount : count;
            for (uint32_t i = 0; i < keep; i++) {
                cache->m_rows[i].m_largestGap = old[i].m_largestGap;
                cache->m_rows[i].m_glyphs.InitEmpty(old[i].m_glyphs.m_linkoffset);
                old[i].m_glyphs.UnlinkAll();
                old[i].m_glyphs.m_terminator.Unlink();
            }
            SMemFree(old, s_rowArrayTag, -2, 0);
        }
        for (uint32_t i = keep; i < count; i++) {
            cache->m_rows[i].m_largestGap = 0;
            cache->m_rows[i].m_pad04 = 0;
            cache->m_rows[i].m_glyphs.InitEmpty(0x30);
        }
        cache->m_rowCount = count;
    }
    for (uint32_t i = 0; i < count; i++) {
        cache->m_rows[i].m_largestGap = cache->m_width;
    }
}

// A cell for a glyph in the first row of the page with room.
// ref: FUN_1406a0400
CharCodeDesc* TextureCacheAllocate(TEXTURECACHE* cache, uint32_t code, GXUGLYPHBITMAP* bitmap, CGxuFont* font) {
    for (uint32_t i = 0; i < cache->m_rowCount; i++) {
        CharCodeDesc* glyph = TextureCacheRowAllocate(&cache->m_rows[i], code, bitmap, i, cache->m_font->m_cellSize,
            cache->m_width, cache->m_rowCount / cache->m_layers, font);
        if (glyph != nullptr) {
            return glyph;
        }
    }
    return nullptr;
}

// Uploads a page that gained glyphs.
// ref: FUN_1406a0cc0
void TextureCacheUpload(TEXTURECACHE* cache) {
    if (cache->m_dirty == 0 || cache->m_texture == nullptr) {
        return;
    }
    auto size = static_cast<const uint16_t*>(cache->m_texture);
    int32_t rect[4] = { 0, 0, size[1], size[0] };
    TEXTURECACHE* param = cache;
    GxTexUpdateInFrame(g_gxRenderContext, cache->m_texture, rect, false, FillPage, &param);
    cache->m_dirty = 0;
}

// The texture, rows and glyphs of a page go (the per-page block of FUN_140697f10).
// ref: FUN_1406885d0
void TextureCacheClear(TEXTURECACHE* cache) {
    void* texture = cache->m_texture;
    cache->m_texture = nullptr;
    if (texture != nullptr) {
        GxTexDestroy(&texture);
    }
    TextureCacheFreeRows(cache);
}

// TSHashTable<CharCodeDesc, HASHKEY_NONE, 8>'s node deletion (FUN_14068c0b0).
// ref: FUN_14068c0b0
void CHARCODETABLE::Delete(CharCodeDesc* node) {
    node->m_recentLink.Unlink();
    node->m_rowLink.Unlink();
    this->Unlink(node);
    node->m_linktofull.Unlink();
    node->m_linktoslot.Unlink();
    SMemFree(node, s_glyphTableTag, -2, 0);
}

// TSList<CharCodeDesc>::NewNode (FUN_14068c2d0).
// ref: FUN_14068c2d0
CharCodeDesc* CHARCODETABLE::NewNode(TSExplicitList<CharCodeDesc>* slot, uint32_t extra, uint32_t flags) {
    void* memory = SMemAlloc(extra + sizeof(CharCodeDesc), 8, s_glyphListTag, 0, -2, flags | 8);
    auto node = new (memory) CharCodeDesc();
    std::memset(node->m_texCoords, 0, sizeof(node->m_texCoords));
    node->m_glyphCode = 0;
    node->m_page = 0xffffffff;
    node->m_row = 0xffffffff;
    node->m_top = 0xffffffff;
    node->m_bottom = 0;
    slot->LinkToTail(node);
    return node;
}

// ref: FUN_140699430
void KERNTABLE::Delete(KernNode* node) {
    this->Unlink(node);
    node->m_linktofull.Unlink();
    node->m_linktoslot.Unlink();
    SMemFree(node, s_kernTableTag, -2, 0);
}

// ref: FUN_140699660
KernNode* KERNTABLE::NewNode(TSExplicitList<KernNode>* slot, uint32_t extra, uint32_t flags) {
    void* memory = SMemAlloc(extra + sizeof(KernNode), 8, s_kernListTag, 0, -2, flags | 8);
    auto node = new (memory) KernNode();
    node->m_pair = 0;
    node->m_step = 0.0f;
    node->m_pad34 = 0;
    slot->LinkToTail(node);
    return node;
}
