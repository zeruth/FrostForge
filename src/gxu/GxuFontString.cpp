// Gxu strings (Engine/Source/Gxu/GxuFontString.cpp): text laid out with a font into quads per
// glyph page, with its colours, shadow, gradient, hyperlinks and embedded textures.

#include <cmath>
#include <cstring>
#include <new>

#include "blz/String.hpp"
#include "gx/Gx.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_stringFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontString.cpp";
const char s_vertexTag[] = ".?AUGxVertexPT@@";
const char s_colorTag[] = ".?AVCArgb@@";
const char s_gradientTag[] = ".?AUGradientInfo@@";
const char s_textureTag[] = ".?AUGxuEmbeddedTextureInfo@@";
const char s_hyperlinkTag[] = ".?AUGxuFontHyperlinkInfo@@";
const char s_textTag[] = "D";

// A blz::vector's storage grown for `count` elements: by half again, at least to `count`.
template <class T>
void VectorReserve(GXUVECTOR<T>* vector, uint64_t count, const char* tag) {
    if (count <= vector->Capacity()) {
        return;
    }
    uint64_t grown = (vector->Capacity() >> 1) + vector->Capacity();
    if (grown < count) {
        grown = count;
    }
    auto data = static_cast<T*>(SMemAlloc(grown * sizeof(T), 0xb, tag, SStrHashFnv1a(tag), -6, 0));
    if (vector->m_size != 0) {
        std::memcpy(data, vector->m_data, vector->m_size * sizeof(T));
    }
    if (vector->Owned() && vector->m_data != nullptr) {
        SMemFree(vector->m_data, tag, -6, 0);
    }
    vector->m_data = data;
    vector->m_capacity = grown;
}

template <class T>
void VectorAppend(GXUVECTOR<T>* vector, uint64_t count, const T& value, const char* tag) {
    VectorReserve(vector, vector->m_size + count, tag);
    for (uint64_t i = 0; i < count; i++) {
        vector->m_data[vector->m_size + i] = value;
    }
    vector->m_size += count;
}

template <class T>
void VectorFree(GXUVECTOR<T>* vector, const char* tag) {
    if (vector->Owned() && vector->m_data != nullptr) {
        SMemFree(vector->m_data, tag, -6, 0);
    }
    vector->m_data = nullptr;
    vector->m_size = 0;
    vector->m_capacity = 0;
}

void FreePage(GXUPAGEGEOMETRY*& page) {
    GXUPAGEGEOMETRY* geometry = page;
    page = nullptr;
    if (geometry == nullptr) {
        return;
    }
    VectorFree(&geometry->m_colors, s_colorTag);
    VectorFree(&geometry->m_vertices, s_vertexTag);
    SMemFree(geometry, s_stringFile, -2, 0);
}

// A string's colour stack: the line's colours as |c and |r push and pop them (a
// blz::vector<CArgb> with room for ten inline).
struct COLORSTACK {
    GXUVECTOR<uint32_t> m_vector;
    uint32_t m_inline[10];

    COLORSTACK() {
        this->m_vector.m_data = this->m_inline;
        this->m_vector.m_size = 0;
        this->m_vector.m_capacity = 0x800000000000000aull;
    }
    ~COLORSTACK() {
        VectorFree(&this->m_vector, s_colorTag);
    }
};

}

// The creation's defaults (FUN_14068a7e0).
CGxString::CGxString() {
    std::memset(this, 0, sizeof(*this));
    this->m_scale = 1.0f;
    this->m_rotation = 0.0f;
    this->m_requestedHeight = 0.02f;
    this->m_height = 0.02f;
    this->m_color = 0xffffffff;
    this->m_shadowColor = 0xff000000;
    this->m_shadowOffset[0] = 1.0f;
    this->m_shadowOffset[1] = -1.0f;
    this->m_width = 1.0f;
    this->m_maxHeight = 1.0f;
    this->m_text = this->m_textBuffer;
    this->m_textCapacity = 0x800000000000000full;
    this->m_gradientStart = -1;
    this->m_gradientLength = -1;
    this->m_textScale = 1.0f;
    this->m_script = 4;
}

// ref: FUN_140688ae0
CGxString::~CGxString() {
    VectorFree(&this->m_gradients, s_gradientTag);
    VectorFree(&this->m_textures, s_textureTag);
    VectorFree(&this->m_hyperlinks, s_hyperlinkTag);
    if (static_cast<int64_t>(this->m_textCapacity) >= 0 && this->m_text != nullptr) {
        SMemFree(this->m_text, s_textTag, -6, 0);
    }
    if (this->m_slug.m_valid != 0) {
        VectorFree(&this->m_slug.m_indices, "G");
        VectorFree(&this->m_slug.m_vertices, ".?AUSlugVertex2D@@");
    }
    this->m_slug.m_valid = 0;
    for (int32_t i = 7; i >= 0; i--) {
        FreePage(this->m_pages[i]);
    }
    this->m_batchLink.Unlink();
    this->m_fontLink.Unlink();
    this->m_link.Unlink();
}

// ref: FUN_140694760
bool CGxString::Initialize(float height, const float* position, float width, float maxHeight, CGxuFont* font,
    const char* text, int32_t justifyV, int32_t justifyH, float spacing, uint32_t flags, const uint32_t* color,
    float textScale, float scale, float rotation) {
    uint64_t length = 0;
    if (*text != '\0') {
        length = std::strlen(text);
    }
    blz::string_assign(reinterpret_cast<blz::string_header*>(&this->m_text), text, length);
    this->m_position[0] = position[0];
    this->m_position[1] = position[1];
    this->m_position[2] = position[2];
    this->m_color = *color;
    this->m_flags = flags;
    this->m_width = width;
    this->m_maxHeight = maxHeight;
    this->m_justifyV = justifyV;
    this->m_spacing = spacing;
    this->m_textScale = textScale;
    this->m_justifyH = justifyH;
    this->m_scale = scale;
    this->m_rotation = rotation;
    this->m_font = font;
    this->m_fontLink.LinkToTail(&font->m_strings);
    if ((flags & 4) != 0 && ((this->m_flags >> 7) & 1) == 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    this->m_requestedHeight = height;
    float minimum = 2.0f / static_cast<float>(g_gxuScreenHeight);
    this->m_height = height <= minimum ? minimum : height;
    return true;
}

// The laid-out geometry goes; the string waits on the plain list to be laid out again.
// ref: FUN_1406936c0
void CGxString::ClearGeometry() {
    this->m_flags &= ~0x4000u;
    this->m_hyperlinks.m_size = 0;
    this->m_textures.m_size = 0;
    this->m_gradients.m_size = 0;
    this->m_lineCount = 0;
    this->m_pageMask = 0;
    this->m_gradientStart = -1;
    this->m_gradientLength = -1;
    this->m_script = 4;
    for (int32_t i = 0; i < 8; i++) {
        FreePage(this->m_pages[i]);
    }
    if (this->m_slug.m_valid != 0) {
        VectorFree(&this->m_slug.m_indices, "G");
        VectorFree(&this->m_slug.m_vertices, ".?AUSlugVertex2D@@");
    }
    this->m_slug.m_valid = 0;
    this->m_link.Unlink();
    this->m_link.LinkToTail(&g_gxuStrings);
}

// Lays the string out again when a page it used changed and a glyph it needs went, then
// makes sure it is laid out.
// ref: FUN_140693600
bool CGxString::CheckGeometry() {
    if (this->m_dirty != 0) {
        if (!this->m_font->HasGlyphs(this->m_text)) {
            this->ClearGeometry();
        }
        this->m_dirty = 0;
    }
    this->BuildGeometry();
    this->m_lastFrame = GxDeviceFrame();
    this->m_font->m_lastFrame = GxDeviceFrame();
    return this->m_lineCount != 0;
}

// A string laid out for the other path (Slug or bitmap) than its font now takes is laid
// out again.
// ref: FUN_140693670
void CGxString::ValidateGeometry() {
    bool slug = this->m_font->UsesSlug();
    if (slug != (((this->m_flags >> 14) & 1) != 0)) {
        this->ClearGeometry();
        this->BuildGeometry();
    }
}

// Lays the text out line by line into the pages' quads.
// ref: FUN_140693a10
void CGxString::BuildGeometry() {
    if (this->m_lineCount != 0 || this->m_textLength == 0) {
        return;
    }
    int32_t gradientStart = this->m_gradientStart;
    int32_t gradientLength = this->m_gradientLength;
    this->m_gradients.m_size = 0;
    this->m_glyphCount = 0;
    float lineHeight = this->m_height * this->m_scale;
    bool scaled = ((this->m_flags >> 7) & 1) != 0;
    float top = GxuFontRoundHeight(scaled, -lineHeight);
    float pen[3] = { 0.0f, top, 0.0f };
    if (scaled) {
        pen[1] = 0.0f;
    }
    float penY = pen[1];
    COLORSTACK colors;
    VectorAppend(&colors.m_vector, 1, this->m_color, s_colorTag);
    const char* text = this->m_text;
    bool slug = this->m_font->UsesSlug();
    if (slug) {
        float width = 0.0f;
        int32_t glyphs = 0;
        GxuSlugCountGeometry(this, &width, &glyphs);
    }
    float spacing = this->m_spacing * this->m_scale;
    GXULINKSTATE link;
    std::memset(&link, 0, sizeof(link));
    float spacingPixels = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(
        static_cast<float>(g_gxuScreenHeight) * spacing + 0.99995f)));
    float heightPixels = GxuFontRoundHeight(false, lineHeight);
    float lineStep;
    if (!scaled) {
        lineStep = heightPixels + spacingPixels;
        spacing = spacingPixels / static_cast<float>(g_gxuScreenHeight);
    } else {
        lineStep = spacing + lineHeight;
        if (slug) {
            lineStep *= 512.0f;
        }
    }
    float width = this->m_width;
    float used = 0.0f;
    uint8_t wrapped = 1;
    this->m_overflow = 0.0f;
    float indent = 0.0f;
    if ((this->m_flags & 1) != 0) {
        indent = this->m_shadowOffset[0];
    }
    if (*text != '\0') {
        while (used < this->m_maxHeight) {
            link.m_rect[0] = lineStep + penY;
            link.m_texture.m_rect[0] = lineStep + penY;
            if (((this->m_flags >> 0xd) & 1) != 0 && this->m_lineCount == 1) {
                width -= g_gxuIndent;
            }
            used += spacing + lineHeight;
            link.m_rect[2] = penY;
            link.m_texture.m_rect[2] = penY;
            if (*text == '\0') {
                break;
            }
            int32_t advance = 0;
            uint32_t code = 0;
            int32_t kind = GxuDetermineControlCode(text, &advance, nullptr, this->m_flags, &code);
            if (kind == 3) {
                text += advance;
            } else {
                int32_t lineLength = 0;
                float lineWidth = 0.0f;
                const char* next = nullptr;
                float textureHeight = 0.0f;
                GxuFontGetLineExtent(this->m_font, text, lineHeight, width, &lineLength, &lineWidth, &next, indent,
                    this->m_flags, &wrapped, &textureHeight, this->m_textScale);
                if (next == text || next == nullptr || (lineLength == 0 && *next == '\0')) {
                    break;
                }
                if (slug && ((this->m_flags >> 7) & 1) != 0) {
                    lineWidth *= 512.0f;
                }
                bool indented = ((this->m_flags >> 0xd) & 1) != 0 && this->m_lineCount != 0;
                float x;
                if (this->m_justifyH == 2) {
                    x = GxuFontRoundWidth(((this->m_flags >> 7) & 1) != 0, -lineWidth) - (indented ? g_gxuLineGap : 0.0f);
                } else if (this->m_justifyH == 1) {
                    x = GxuFontRoundWidth(((this->m_flags >> 7) & 1) != 0, -(lineWidth / 2.0f));
                } else {
                    x = indented ? g_gxuLineGap : 0.0f;
                }
                float extra = 0.0f;
                if (heightPixels < textureHeight) {
                    extra = textureHeight - heightPixels;
                    this->m_overflow += extra;
                }
                if (this->m_justifyV == 1) {
                    extra = floorf(extra * 0.5f);
                }
                if (this->m_justifyV != 0) {
                    penY -= extra;
                    link.m_rect[2] -= extra;
                    link.m_rect[0] -= extra;
                    link.m_texture.m_rect[2] -= extra;
                    link.m_texture.m_rect[0] -= extra;
                    pen[1] = penY;
                }
                pen[0] = x;
                uint32_t pages = 0;
                if (link.m_state != 0) {
                    link.m_rect[1] = x;
                }
                this->WriteGeometry(text, lineLength, &colors.m_vector, pen, &pages, &link, slug);
                if (this->m_justifyV != 2) {
                    link.m_rect[0] -= extra;
                    link.m_rect[2] -= extra;
                    link.m_texture.m_rect[0] -= extra;
                    penY -= extra;
                    link.m_texture.m_rect[2] -= extra;
                }
                if (static_cast<uint32_t>(link.m_state - 1) < 2) {
                    GXUHYPERLINKINFO info;
                    info.m_rect[0] = link.m_rect[0] / static_cast<float>(g_gxuScreenHeight);
                    info.m_rect[1] = link.m_rect[1] / static_cast<float>(g_gxuScreenWidth);
                    info.m_rect[2] = link.m_rect[2] / static_cast<float>(g_gxuScreenHeight);
                    info.m_rect[3] = link.m_rect[3] / static_cast<float>(g_gxuScreenWidth);
                    info.m_link = link.m_link;
                    info.m_linkLength = link.m_linkLength;
                    info.m_pad1c = link.m_pad24;
                    info.m_text = link.m_text;
                    info.m_textLength = link.m_textLength;
                    info.m_pad2c = link.m_pad34;
                    info.m_color = link.m_color;
                    info.m_colorLength = link.m_colorLength;
                    info.m_pad3c = link.m_pad44;
                    VectorAppend(&this->m_hyperlinks, 1, info, s_hyperlinkTag);
                }
                this->m_pageMask |= pages;
                text = next;
            }
            this->m_lineCount++;
            if ((this->m_flags & 2) != 0) {
                break;
            }
            penY -= lineStep;
            pen[1] = penY;
            if (*text == '\0') {
                break;
            }
        }
    }
    this->ComputePosition();
    if ((this->m_flags & 0x20) != 0 && (gradientStart != -1 || gradientLength != -1)) {
        this->SetGradient(gradientStart, gradientLength);
    }
    if (this->m_lineCount != 0) {
        this->m_link.Unlink();
        this->m_link.LinkToTail(&g_gxuStringsWithGeometry);
    }
}

// One line's quads: each glyph's cell placed after the last, kerned, in the colour on top of
// the stack; hyperlinks and textures noted as they pass.
// ref: FUN_140694910
void CGxString::WriteGeometry(const char* text, int32_t length, GXUVECTOR<uint32_t>* colors, float* pen,
    uint32_t* pages, GXULINKSTATE* link, bool slug) {
    float sine = 0.0f;
    float cosine = 1.0f;
    float negativeSine = 0.0f;
    bool rotated = 0.001f <= fabsf(this->m_rotation - 0.0f);
    if (rotated) {
        sine = sinf(this->m_rotation);
        cosine = cosf(this->m_rotation);
        negativeSine = -sine;
    }
    uint32_t previous = 0;
    float lineHeight = this->m_height * this->m_scale;
    if ((this->m_flags & 8) != 0) {
        for (GXUPAGEGEOMETRY* page : this->m_pages) {
            if (page != nullptr) {
                page->m_colors.m_size = 0;
            }
        }
    }
    float penX = pen[0];
    float penY = pen[1];
    float penZ = pen[2];
    bool scaled = ((this->m_flags >> 7) & 1) != 0;
    float height = lineHeight;
    if (slug && scaled) {
        height = lineHeight * 512.0f;
    }
    float heightPixels = GxuFontRoundHeight(scaled, height);
    float scale = heightPixels / static_cast<float>(this->m_font->GetPixelSize());
    float cellHeight = heightPixels;
    if ((this->m_font->m_flags & 8) != 0) {
        cellHeight = heightPixels + 4.0f;
    } else if ((this->m_font->m_flags & 1) != 0) {
        cellHeight = heightPixels + 2.0f;
    }
    if (static_cast<uint32_t>(link->m_state - 1) < 2) {
        link->m_rect[3] = penX;
    }
    float kern = 0.0f;
    float step = 0.0f;
    float x = penX;
    if (*text != '\0') {
        float y = penY;
        do {
            if (length == 0) {
                break;
            }
            int32_t advance = 0;
            uint32_t color = 0;
            uint32_t code = 0;
            int32_t kind = GxuDetermineControlCode(text, &advance, &color, this->m_flags, &code);
            text += advance;
            length -= advance;
            if (previous != 0) {
                if ((this->m_flags & 0x10) == 0) {
                    kern = this->m_font->GetKernedAdvance(previous, code);
                } else {
                    kern = this->m_font->GetMonoAdvance(previous, code);
                }
            }
            step = kern * scale;
            switch (kind) {
            case 1:
                if ((this->m_flags & 8) == 0) {
                    color = (color & 0x00ffffff) | (this->m_color & 0xff000000);
                    if (colors->m_size < colors->Capacity()) {
                        VectorAppend(colors, 1, color, s_colorTag);
                    }
                }
                link->m_colorLength = advance;
                link->m_color = text - advance;
                break;
            case 2:
                if ((this->m_flags & 8) == 0 && 1 < colors->m_size) {
                    colors->m_size--;
                }
                break;
            case 3: case 8: case 9: case 10: case 11: case 12:
                break;
            case 5:
                if (link->m_state == 0) {
                    link->m_state = 1;
                    link->m_link = text + (2 - advance);
                    link->m_linkLength = (advance - 2) - 2;
                    link->m_text = text - advance;
                    const char* end = std::strstr(text, "|h");
                    int32_t textLength = 0;
                    if (end != nullptr) {
                        const char* after = end + 2;
                        if (std::strncmp(after, "|r", 2) == 0 && link->m_color != nullptr) {
                            after += 2;
                            link->m_text = link->m_color;
                            link->m_color = nullptr;
                            link->m_colorLength = 0;
                        }
                        textLength = static_cast<int32_t>(after - link->m_text);
                    }
                    link->m_textLength = textLength;
                    link->m_rect[3] = step + x;
                    link->m_rect[1] = step + x;
                }
                break;
            case 6:
                if (static_cast<uint32_t>(link->m_state - 1) < 2) {
                    float right = link->m_rect[3];
                    if (right <= step + x) {
                        right = step + x;
                    }
                    link->m_rect[3] = right;
                    link->m_state = 0;
                    GXUHYPERLINKINFO info;
                    info.m_rect[0] = link->m_rect[0] / static_cast<float>(g_gxuScreenHeight);
                    info.m_rect[1] = link->m_rect[1] / static_cast<float>(g_gxuScreenWidth);
                    info.m_rect[2] = link->m_rect[2] / static_cast<float>(g_gxuScreenHeight);
                    info.m_rect[3] = link->m_rect[3] / static_cast<float>(g_gxuScreenWidth);
                    info.m_link = link->m_link;
                    info.m_linkLength = link->m_linkLength;
                    info.m_pad1c = link->m_pad24;
                    info.m_text = link->m_text;
                    info.m_textLength = link->m_textLength;
                    info.m_pad2c = link->m_pad34;
                    info.m_color = link->m_color;
                    info.m_colorLength = link->m_colorLength;
                    info.m_pad3c = link->m_pad44;
                    VectorAppend(&this->m_hyperlinks, 1, info, s_hyperlinkTag);
                }
                break;
            case 7:
            case 13: {
                if (link->m_state == 2) {
                    break;
                }
                GXUEMBEDDEDTEXTURE* texture = &link->m_texture;
                bool parsed = kind == 13
                    ? GxuFontParseAtlasCode(text - advance, texture, heightPixels, this->m_textScale, height)
                    : GxuFontParseTextureCode(text - advance, texture, heightPixels, this->m_textScale, height);
                if (!parsed) {
                    break;
                }
                float saved[4] = { texture->m_rect[0], texture->m_rect[1], texture->m_rect[2], texture->m_rect[3] };
                float gap = 0.0f;
                if (previous != 0) {
                    float before = 0.0f;
                    CharCodeDesc* glyph = this->m_font->GetGlyph(previous);
                    if (glyph != nullptr) {
                        before = this->m_font->GetBearing(glyph, ((this->m_flags >> 7) & 1) != 0, height)
                            + static_cast<float>(glyph->m_width);
                    }
                    gap = 0.0f;
                    if (gap <= before * scale) {
                        gap = before * scale;
                    }
                    if ((this->m_font->m_flags & 8) != 0) {
                        gap += scale * 4.0f;
                    } else if ((this->m_font->m_flags & 1) != 0) {
                        gap += scale * 2.0f;
                    }
                }
                float right = scale * texture->m_height + gap + x;
                texture->m_rect[1] = gap + x;
                texture->m_rect[3] = right;
                if (this->m_justifyV == 0) {
                    texture->m_rect[2] = texture->m_rect[0] - scale * texture->m_width;
                } else if (this->m_justifyV == 2) {
                    texture->m_rect[0] = scale * texture->m_width + texture->m_rect[2];
                } else if (this->m_justifyV == 1) {
                    float top = (texture->m_width * scale - heightPixels) * 0.5f + texture->m_rect[0];
                    texture->m_rect[0] = top;
                    texture->m_rect[2] = top - texture->m_width * scale;
                }
                x = floorf(right + 0.5f);
                if (link->m_state == 1) {
                    if (texture->m_rect[0] <= link->m_rect[0]) {
                        link->m_rect[0] = texture->m_rect[0];
                    }
                    if (texture->m_rect[1] <= link->m_rect[1]) {
                        link->m_rect[1] = texture->m_rect[1];
                    }
                    if (link->m_rect[2] <= texture->m_rect[2]) {
                        link->m_rect[2] = texture->m_rect[2];
                    }
                    if (link->m_rect[3] <= texture->m_rect[3]) {
                        link->m_rect[3] = texture->m_rect[3];
                    }
                    link->m_state = 2;
                }
                GXUEMBEDDEDTEXTURE kept = *texture;
                kept.m_rect[0] = texture->m_rect[0] / static_cast<float>(g_gxuScreenHeight);
                kept.m_rect[1] = texture->m_rect[1] / static_cast<float>(g_gxuScreenWidth);
                kept.m_rect[2] = texture->m_rect[2] / static_cast<float>(g_gxuScreenHeight);
                kept.m_rect[3] = texture->m_rect[3] / static_cast<float>(g_gxuScreenWidth);
                kept.m_width = texture->m_width / static_cast<float>(g_gxuScreenHeight);
                kept.m_height = texture->m_height / static_cast<float>(g_gxuScreenWidth);
                kept.m_offsetX = texture->m_offsetX / static_cast<float>(g_gxuScreenWidth);
                kept.m_offsetY = texture->m_offsetY / static_cast<float>(g_gxuScreenHeight);
                VectorAppend(&this->m_textures, 1, kept, s_textureTag);
                texture->m_rect[0] = saved[0];
                texture->m_rect[1] = saved[1];
                texture->m_rect[2] = saved[2];
                texture->m_rect[3] = saved[3];
                kern = 0.0f;
                previous = 0;
                break;
            }
            default: {
                CharCodeDesc* glyph = this->m_font->GetGlyph(code);
                if (glyph == nullptr) {
                    if (this->m_script == 4) {
                        this->m_script = GxuFontScriptOf(code);
                    }
                    code = 0xfffd;
                    glyph = this->m_font->GetGlyph(0xfffd);
                    if (glyph == nullptr) {
                        break;
                    }
                    if (previous != 0) {
                        if ((this->m_flags & 0x10) == 0) {
                            kern = this->m_font->GetKernedAdvance(previous, 0xfffd);
                        } else {
                            kern = this->m_font->GetMonoAdvance(previous, 0xfffd);
                        }
                        step = kern * scale;
                    }
                }
                uint32_t pageIndex = glyph->m_page;
                TEXTURECACHE* cache = &this->m_font->m_pages[pageIndex];
                uint32_t layers = cache->m_layers;
                uint32_t rowLength = cache->m_width;
                uint32_t layer = glyph->m_row / (cache->m_rowCount / layers);
                *pages |= 1u << (pageIndex & 0x1f);
                if (this->m_pages[pageIndex] == nullptr) {
                    auto geometry = static_cast<GXUPAGEGEOMETRY*>(SMemAlloc(sizeof(GXUPAGEGEOMETRY), 0x1b, s_stringFile,
                        SStrHashFnv1a(s_stringFile), 0x14e, 0));
                    std::memset(geometry, 0, sizeof(*geometry));
                    this->m_pages[pageIndex] = geometry;
                }
                GXUPAGEGEOMETRY* geometry = this->m_pages[pageIndex];
                if ((this->m_flags & 8) == 0) {
                    uint32_t base = static_cast<uint32_t>(geometry->m_colors.m_size);
                    uint32_t current = colors->m_data[colors->m_size - 1];
                    VectorAppend(&geometry->m_colors, 4, current, s_colorTag);
                    if ((this->m_flags & 0x20) != 0) {
                        for (uint32_t i = 0; i < 4; i++) {
                            GXUGRADIENTINFO info;
                            info.m_page = static_cast<uint8_t>(pageIndex);
                            std::memset(info.m_pad, 0, sizeof(info.m_pad));
                            info.m_vertex = i + base;
                            VectorAppend(&this->m_gradients, 1, info, s_gradientTag);
                        }
                    }
                }
                if (!slug) {
                    float x0 = glyph->m_texCoords[0];
                    float y0 = glyph->m_texCoords[1];
                    float x1 = glyph->m_texCoords[2];
                    float y1 = glyph->m_texCoords[3];
                    float glyphWidth = static_cast<float>(glyph->m_width) * scale;
                    float glyphHeight = heightPixels;
                    if (!scaled) {
                        glyphWidth = floorf(glyphWidth);
                        step = floorf(step + 0.5f);
                        glyphHeight = cellHeight;
                    }
                    if ((this->m_font->m_flags & 4) != 0) {
                        x0 += g_gxuPadding[0];
                        y0 += g_gxuPadding[1];
                        x1 += g_gxuPadding[2];
                        y1 += g_gxuPadding[3];
                    }
                    x += step;
                    GXVERTEXPT corner;
                    corner.m_x = this->m_font->GetBearing(glyph, scaled, height) + x;
                    corner.m_y = y;
                    if ((this->m_font->m_flags & 8) != 0) {
                        corner.m_y = y - 2.0f;
                    } else if ((this->m_font->m_flags & 1) != 0) {
                        corner.m_y = y - 1.0f;
                    }
                    corner.m_y += static_cast<float>(glyph->m_yOffset) * scale;
                    corner.m_z = penZ;
                    corner.m_u = 0.0f;
                    corner.m_v = 0.0f;
                    uint64_t base = geometry->m_vertices.m_size;
                    VectorAppend(&geometry->m_vertices, 4, corner, s_vertexTag);
                    GXVERTEXPT* v = &geometry->m_vertices.m_data[base];
                    float right = glyphWidth + v[3].m_x;
                    float bottom = glyphHeight + v[3].m_y;
                    v[3].m_x = right;
                    v[2].m_x = right;
                    v[3].m_y = bottom;
                    v[1].m_y = bottom;
                    if (rotated) {
                        float px = v[0].m_x;
                        float py = v[0].m_y;
                        v[0].m_x = py * negativeSine + px * cosine;
                        v[0].m_y = py * cosine + px * sine;
                        px = v[1].m_x;
                        v[1].m_x = bottom * negativeSine + px * cosine;
                        v[1].m_y = bottom * cosine + px * sine;
                        py = v[2].m_y;
                        v[2].m_x = py * negativeSine + right * cosine;
                        v[2].m_y = py * cosine + right * sine;
                        v[3].m_x = bottom * negativeSine + right * cosine;
                        v[3].m_y = bottom * cosine + right * sine;
                    }
                    v[2].m_v = x1;
                    v[0].m_v = x1;
                    v[3].m_v = x0;
                    v[1].m_v = x0;
                    float u0 = (static_cast<float>(rowLength * layer) + y0) / static_cast<float>(layers);
                    v[1].m_u = u0;
                    v[0].m_u = u0;
                    float u1 = (static_cast<float>(rowLength * layer) + y1) / static_cast<float>(layers);
                    v[3].m_u = u1;
                    v[2].m_u = u1;
                    this->m_glyphCount++;
                    this->m_flags &= ~0x4000u;
                } else {
                    x += step;
                    GxuSlugWriteGlyph(nullptr, &this->m_slug);
                    this->m_flags |= 0x4000;
                    this->m_glyphCount++;
                }
                previous = code;
                break;
            }
            }
        } while (*text != '\0');
    }
    link->m_rect[3] = step + x;
}

// The string's screen position: its anchor moved by the justification and snapped to pixels.
// ref: FUN_140695c70
void CGxString::ComputePosition() {
    int32_t lines = this->m_lineCount;
    this->m_drawPosition[0] = this->m_position[0];
    this->m_drawPosition[1] = this->m_position[1];
    this->m_drawPosition[2] = this->m_position[2];
    if (lines == 0 || (this->m_flags & 0x80) != 0) {
        return;
    }
    if (this->m_justifyH == 2) {
        this->m_drawPosition[0] += this->m_width;
    } else if (this->m_justifyH == 1) {
        this->m_drawPosition[0] += this->m_width * 0.5f;
    }
    float spacing = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(
                        static_cast<float>(g_gxuScreenHeight) * this->m_spacing + 0.99995f)))
        / static_cast<float>(g_gxuScreenHeight);
    float height = static_cast<float>(lines) * this->m_height + static_cast<float>(lines - 1) * spacing
        + this->m_overflow / static_cast<float>(g_gxuScreenHeight);
    if (this->m_justifyV == 0) {
        this->m_drawPosition[1] += this->m_maxHeight;
    } else if (this->m_justifyV == 1) {
        this->m_drawPosition[1] += (this->m_maxHeight - height) * 0.5f + height;
    } else {
        this->m_drawPosition[1] += height;
    }
    this->m_drawPosition[0] = floorf(static_cast<float>(g_gxuScreenWidth) * this->m_drawPosition[0]);
    this->m_drawPosition[1] = floorf(static_cast<float>(g_gxuScreenHeight) * this->m_drawPosition[1]);
}

// ref: FUN_140695e20
void CGxString::SetColor(const uint32_t* color) {
    if (*color == this->m_color) {
        return;
    }
    this->m_color = *color;
    uint8_t alpha = static_cast<uint8_t>(*color >> 24);
    uint8_t shadow = static_cast<uint8_t>(this->m_shadowColor >> 24);
    if (shadow <= alpha) {
        alpha = shadow;
    }
    this->m_shadowColor = (this->m_shadowColor & 0x00ffffff) | (static_cast<uint32_t>(alpha) << 24);
    if ((this->m_flags & 0x28) != 8 || this->m_gradients.m_size != 0 || ((this->m_flags >> 0xe) & 1) != 0) {
        this->ClearGeometry();
    }
}

// The shadow's colour (no more opaque than the text) and offset.
// ref: FUN_140693570
void CGxString::SetShadow(const float* offset, const uint32_t* color) {
    this->m_shadowColor = *color;
    uint8_t alpha = static_cast<uint8_t>(*color >> 24);
    uint8_t text = static_cast<uint8_t>(this->m_color >> 24);
    if (text <= alpha) {
        alpha = text;
    }
    this->m_shadowColor = (this->m_shadowColor & 0x00ffffff) | (static_cast<uint32_t>(alpha) << 24);
    this->m_flags |= 1;
    this->m_shadowOffset[0] = offset[0];
    this->m_shadowOffset[1] = offset[1];
    if ((this->m_flags & 0x20) != 0) {
        this->ClearGeometry();
    }
}

// A gradient: glyphs before `start` keep their alpha, the next `length` fade out, the rest
// are hidden. 0 when it stays inside the string, 1 when it runs past its end, 2 when it
// does not apply, -1 when the string has no geometry.
// ref: FUN_140695e70
int32_t CGxString::SetGradient(int32_t start, int32_t length) {
    if (start < 0 || length < 0) {
        return 2;
    }
    if (start == this->m_gradientStart && length == this->m_gradientLength) {
        return 0;
    }
    this->m_gradientStart = start;
    this->m_gradientLength = length;
    this->m_flags = (this->m_flags & ~8u) | 0x20;
    if (this->m_gradients.m_data == nullptr || this->m_gradients.m_size == 0) {
        this->ClearGeometry();
        this->m_gradientStart = start;
        this->m_gradientLength = length;
        if (this->m_dirty != 0) {
            if (!this->m_font->HasGlyphs(this->m_text)) {
                this->ClearGeometry();
            }
            this->m_dirty = 0;
        }
        this->BuildGeometry();
        this->m_lastFrame = GxDeviceFrame();
        this->m_font->m_lastFrame = GxDeviceFrame();
    }
    if (this->m_lineCount == 0) {
        return -1;
    }
    uint32_t alpha = static_cast<uint8_t>(this->m_color >> 24);
    uint8_t step = static_cast<uint8_t>(alpha);
    if (length != 0) {
        step = static_cast<uint8_t>(alpha / static_cast<uint32_t>(length));
    }
    int32_t count = static_cast<int32_t>(this->m_gradients.m_size);
    int32_t first = start * 4;
    int32_t index = 0;
    for (int64_t i = 0; i < count; i++, index++) {
        uint8_t current = static_cast<uint8_t>(alpha);
        GXUGRADIENTINFO& info = this->m_gradients.m_data[i];
        GXUPAGEGEOMETRY* page = info.m_page < 8 ? this->m_pages[info.m_page] : nullptr;
        bool inRange = page != nullptr && info.m_vertex < page->m_colors.m_size;
        if (i < first) {
            if (inRange) {
                reinterpret_cast<uint8_t*>(&page->m_colors.m_data[info.m_vertex])[3] = current;
            }
        } else if (current == 0 || length * 4 + first <= index) {
            if (!inRange || reinterpret_cast<uint8_t*>(&page->m_colors.m_data[info.m_vertex])[3] == 0) {
                break;
            }
            reinterpret_cast<uint8_t*>(&page->m_colors.m_data[info.m_vertex])[3] = current;
        } else {
            if (inRange) {
                reinterpret_cast<uint8_t*>(&page->m_colors.m_data[info.m_vertex])[3] = current;
            }
            if ((index & 1) != 0) {
                alpha = step < current ? static_cast<uint8_t>(current - step) : 0;
            }
        }
    }
    if (first < count) {
        return count < length * 4 + first ? 1 : 0;
    }
    return 2;
}

// ref: FUN_1406960c0
void CGxString::SetPosition(const float* position) {
    this->m_position[0] = position[0];
    this->m_position[1] = position[1];
    this->m_position[2] = position[2];
    this->ComputePosition();
}

// ref: FUN_1406960e0
void CGxString::PageChanged(uint32_t page) {
    if (((this->m_pageMask >> (page & 0x1f)) & 1) != 0) {
        this->m_dirty = 1;
    }
}

// Geometry nobody drew for thirty frames goes.
// ref: FUN_140696100
void CGxString::ReleaseIfIdle() {
    if (this->m_lineCount != 0 && 0x1e < GxDeviceFrame() - this->m_lastFrame) {
        this->ClearGeometry();
    }
}

// The height again after the screen or font changed, and the layout dropped.
// ref: FUN_1406946e0
void CGxString::Recalculate() {
    if ((this->m_flags & 4) != 0) {
        this->m_requestedHeight = GxuFontGetOneToOneHeight(this->m_font);
    }
    float minimum = 2.0f / static_cast<float>(g_gxuScreenHeight);
    this->m_height = this->m_requestedHeight <= minimum ? minimum : this->m_requestedHeight;
    this->ClearGeometry();
}

// ref: FUN_1406935e0
int32_t CGxString::PageVertexCount(int32_t page) {
    int32_t count = static_cast<int32_t>(this->m_pages[page]->m_vertices.m_size);
    if ((this->m_flags & 1) != 0) {
        count *= 2;
    }
    return count;
}

// A page's vertices into a draw: the shadow pass first (when the string has one), then the
// text, `count` from `first` (FUN_14069e770).
// ref: FUN_140696130
void CGxString::RenderPage(uint8_t* vertices, const float* transform, int32_t page, int32_t first, int32_t count,
    const float* texScale) {
    GXUPAGEGEOMETRY* geometry = this->m_pages[page];
    if (geometry == nullptr || count == 0) {
        return;
    }
    int32_t total = static_cast<int32_t>(geometry->m_vertices.m_size);
    if (total == 0) {
        return;
    }
    bool shadow = (this->m_flags & 1) != 0;
    bool gradient = ((this->m_flags >> 5) & 1) != 0;
    int32_t perVertex = static_cast<int32_t>(geometry->m_colors.m_size);
    if (perVertex != 0 && perVertex != total) {
        perVertex = 0;
    }
    int32_t original = first;
    uint8_t* out = vertices;
    if (shadow) {
        if (first < total) {
            int32_t drawn = total - first;
            if (count < drawn) {
                drawn = count;
            }
            float offset[3] = { 0.0f, 0.0f, 0.0f };
            if (g_gxuWorldText == 0) {
                offset[0] = floorf(this->m_shadowOffset[0]) + this->m_drawPosition[0];
                offset[1] = floorf(this->m_shadowOffset[1]) + this->m_drawPosition[1];
                offset[2] = this->m_drawPosition[2];
            } else {
                // World text places its vertices through the batch's transform.
                FROST_UNIMPLEMENTED();
                (void)transform;
            }
            const uint32_t* alphaSource = perVertex != 0 ? &geometry->m_colors.m_data[first] : &this->m_color;
            uint32_t shadowColor = this->m_shadowColor;
            uint8_t alpha = static_cast<uint8_t>(shadowColor >> 24);
            if (gradient && perVertex != 0) {
                alpha = static_cast<uint8_t>(static_cast<int32_t>(
                    static_cast<float>((shadowColor >> 24) * reinterpret_cast<const uint8_t*>(alphaSource)[3]) / 65536.0f));
            }
            for (int32_t i = 0; i < drawn; i++) {
                const GXVERTEXPT& v = geometry->m_vertices.m_data[first + i];
                auto position = reinterpret_cast<float*>(out);
                position[0] = v.m_x + offset[0];
                position[1] = v.m_y + offset[1];
                position[2] = v.m_z + offset[2];
                out[0xc] = static_cast<uint8_t>(shadowColor >> 16);
                out[0xd] = static_cast<uint8_t>(shadowColor >> 8);
                out[0xe] = static_cast<uint8_t>(shadowColor);
                out[0xf] = alpha;
                auto uv = reinterpret_cast<float*>(out + 0x10);
                uv[0] = v.m_u * texScale[0];
                uv[1] = v.m_v * texScale[1];
                out += 0x18;
            }
            count -= drawn;
            first = first + drawn < total ? original : 0;
            if (count == 0) {
                return;
            }
        } else {
            first -= total;
        }
    }
    if (total <= first) {
        return;
    }
    int32_t drawn = total - first;
    if (count < drawn) {
        drawn = count;
    }
    const uint32_t* color = &this->m_color;
    int32_t stride = 0;
    if (perVertex != 0) {
        color = &geometry->m_colors.m_data[first];
        stride = 1;
    }
    if (g_gxuWorldText != 0) {
        FROST_UNIMPLEMENTED();
    }
    for (int32_t i = 0; i < drawn; i++) {
        const GXVERTEXPT& v = geometry->m_vertices.m_data[first + i];
        auto position = reinterpret_cast<float*>(out);
        position[0] = v.m_x + this->m_drawPosition[0];
        position[1] = v.m_y + this->m_drawPosition[1];
        position[2] = v.m_z + this->m_drawPosition[2];
        uint32_t value = *color;
        color += stride;
        out[0xe] = static_cast<uint8_t>(value);
        out[0xf] = static_cast<uint8_t>(value >> 24);
        out[0xd] = static_cast<uint8_t>(value >> 8);
        out[0xc] = static_cast<uint8_t>(value >> 16);
        auto uv = reinterpret_cast<float*>(out + 0x10);
        uv[0] = v.m_u * texScale[0];
        uv[1] = v.m_v * texScale[1];
        out += 0x18;
    }
}

// ref: FUN_1406961f0
void CGxString::RenderSlug(uint8_t* vertices, uint16_t* indices, const float* transform, int32_t firstVertex,
    int32_t vertexCount, int32_t firstIndex, int32_t indexCount, int32_t baseVertex) {
    if (this->m_slug.m_valid != 0) {
        GxuSlugFillGeometry(&this->m_slug, vertices, indices, transform, firstVertex, vertexCount, firstIndex,
            indexCount, baseVertex);
    }
}

// ref: FUN_140694680
int32_t CGxString::GetHyperlinks(GXUHYPERLINKINFO** links) {
    this->BuildGeometry();
    int32_t count = static_cast<int32_t>(this->m_hyperlinks.m_size);
    if (count != 0 && g_gxuScreenWidth != 0 && g_gxuScreenHeight != 0) {
        *links = this->m_hyperlinks.m_data;
        return count;
    }
    return 0;
}

// ref: FUN_140694620
int32_t CGxString::GetTextures(GXUEMBEDDEDTEXTURE** textures) {
    this->BuildGeometry();
    int32_t count = static_cast<int32_t>(this->m_textures.m_size);
    if (count != 0 && g_gxuScreenWidth != 0 && g_gxuScreenHeight != 0) {
        *textures = this->m_textures.m_data;
        return count;
    }
    return 0;
}
