// Gxu fonts' module (Engine/Source/Gxu/GxuFontMain.cpp): its start and end, the device and
// screen it follows, control codes, measuring, and the font, string and batch entry points.

#include "gxu/GxuFont.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"

GXULINK g_gxuFonts = { &g_gxuFonts, &g_gxuFonts };
GXULINK g_gxuStrings = { &g_gxuStrings, &g_gxuStrings };
GXULINK g_gxuStringsWithGeometry = { &g_gxuStringsWithGeometry, &g_gxuStringsWithGeometry };
void* g_gxuFreeTypeLibrary;
uint32_t g_gxuScreenWidth;
uint32_t g_gxuScreenHeight;
float g_gxuLineGap;
float g_gxuIndent;
GxBuf* g_gxuQuadIndices;
uint8_t g_gxuWorldText;
uint8_t g_gxuHasClip;
float g_gxuClip[4];
float g_gxuPadding[4];

void GxuFontCreatePipelines();
void GxuFontCreateSlugPipelines();
void GxuFontDestroyPipelines(void* param);
void GxuFontCreateQuadIndices();
CHandleObject* GxuFontFaceCreateFromBuffer(const char* path, void* library, void* data, uint32_t size);

namespace {

const char s_mainFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontMain.cpp";

GXUCOLORNAMELOOKUP s_colorNameLookup;   // 0x142e9cf80
void* s_stringRenderer;                 // 0x142e9cf78
uint32_t s_lastSize[2];                 // 0x142e9cf88

// The memory FreeType allocates through (0x142cf5e10).
FT_MemoryRec_ s_freeTypeMemory = {
    nullptr,
    [](FT_Memory memory, long size) { return GxuFontFreeTypeAlloc(memory, size); },
    [](FT_Memory memory, void* block) { GxuFontFreeTypeFree(memory, block); },
    [](FT_Memory memory, long current, long size, void* block) {
        return GxuFontFreeTypeRealloc(memory, current, size, block);
    },
};

// A string's link back to the string.
CGxString* StringOf(GXULINK* link) {
    return reinterpret_cast<CGxString*>(link);
}

CGxuFont* FontOf(GXULINK* link) {
    return reinterpret_cast<CGxuFont*>(link);
}

// The codes GxuFontStripCodes knows: the flag that keeps each, its letter and whether a '|'
// goes before it (DAT_1426d2a50).
struct STRIPCODE {
    uint32_t m_flag;
    char m_letter;
    uint8_t m_pad[3];
    uint32_t m_pipe;
};
const STRIPCODE s_stripCodes[9] = {
    { 0x0, '-', { 0, 0, 0 }, 1 },
    { 0x100, 'C', { 0, 0, 0 }, 1 },
    { 0x100, 'R', { 0, 0, 0 }, 1 },
    { 0x200, '\n', { 0, 0, 0 }, 0 },
    { 0x800, '|', { 0, 0, 0 }, 0 },
    { 0x400, 'H', { 0, 0, 0 }, 1 },
    { 0x400, 'h', { 0, 0, 0 }, 1 },
    { 0x1000, 'T', { 0, 0, 0 }, 1 },
    { 0x1000, 't', { 0, 0, 0 }, 1 },
};

// The screen the fonts are sized for; a change rebuilds every font.
void SetScreenSize(const uint32_t* size) {
    if (size[0] == 0 || size[1] == 0 || (size[0] == s_lastSize[0] && size[1] == s_lastSize[1])) {
        return;
    }
    s_lastSize[0] = size[0];
    s_lastSize[1] = size[1];
    g_gxuScreenWidth = size[0];
    g_gxuScreenHeight = size[1];
    g_gxuLineGap = 15.0f;
    g_gxuIndent = 15.0f / static_cast<float>(g_gxuScreenWidth);
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        FontOf(link)->Rebuild();
    }
}

// The device's pipelines, quad indices and Slug textures (FUN_140689330).
// ref: FUN_140689330
void OnDeviceCreated(void* param) {
    (void)param;
    GxuFontCreatePipelines();
    GxuFontCreateSlugPipelines();
    GxuFontCreateQuadIndices();
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        FontOf(link)->PrepareSlug();
    }
}

// The pipelines again when multisampling changes.
// ref: FUN_14068c430
void OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)param;
    if (previous->m_msaaSamples == current->m_msaaSamples) {
        return;
    }
    GxuFontDestroyPipelines(nullptr);
    GxuFontCreatePipelines();
    GxuFontCreateSlugPipelines();
    GxuFontCreateQuadIndices();
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        FontOf(link)->PrepareSlug();
    }
}

// The empty callbacks FUN_14068c520 registers.
void NoDeviceCallback(void* param) {
    (void)param;
}

}

// ref: FUN_14068b4c0
void GxuFontInitialize(const uint32_t* size) {
    GxAddDeviceCreatedCallback(OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(GxuFontDestroyPipelines, nullptr);
    RenderSettingsAddHandler(OnRenderSettingsChanged, nullptr);
    FT_Library library = nullptr;
    FT_New_Library(&s_freeTypeMemory, &library);
    g_gxuFreeTypeLibrary = library;
    FT_Add_Default_Modules(library);
    SetScreenSize(size);
}

// ref: FUN_14068bc90
void GxuFontWindowSizeChanged(const uint32_t* size) {
    SetScreenSize(size);
}

// ref: FUN_14068b7c0
void GxuFontDestroy() {
    GxRemoveDeviceCreatedCallback(OnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(GxuFontDestroyPipelines, nullptr);
    RenderSettingsRemoveHandler(OnRenderSettingsChanged, nullptr);
    GxuFontDestroyPipelines(nullptr);
    while (g_gxuStrings.m_next != &g_gxuStrings) {
        CGxString* string = StringOf(g_gxuStrings.m_next);
        string->~CGxString();
        SMemFree(string, s_mainFile, -2, 0);
    }
    while (g_gxuStringsWithGeometry.m_next != &g_gxuStringsWithGeometry) {
        CGxString* string = StringOf(g_gxuStringsWithGeometry.m_next);
        string->~CGxString();
        SMemFree(string, s_mainFile, -2, 0);
    }
    while (g_gxuFonts.m_next != &g_gxuFonts) {
        CGxuFont* font = FontOf(g_gxuFonts.m_next);
        font->m_link.Unlink();
        font->~CGxuFont();
        SMemFree(font, s_mainFile, -2, 0);
    }
    GxuFontFaceCloseAll();
    if (g_gxuFreeTypeLibrary != nullptr) {
        FT_Done_Library(static_cast<FT_Library>(g_gxuFreeTypeLibrary));
    }
    g_gxuFreeTypeLibrary = nullptr;
}

// ref: FUN_14068bc30
void GxuFontUpdate() {
    for (GXULINK* link = g_gxuStringsWithGeometry.m_next; link != &g_gxuStringsWithGeometry;) {
        GXULINK* next = link->m_next;
        StringOf(link)->ReleaseIfIdle();
        link = next;
    }
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        FontOf(link)->SwapEvicted();
    }
}

// ref: FUN_14068aae0
void GxuFontReleaseIdleFonts() {
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        CGxuFont* font = FontOf(link);
        if ((font->m_flags & 0x60) == 0 && 0x3c < GxDeviceFrame() - font->m_lastFrame) {
            font->ClearGlyphs();
            font->m_flags |= 0x20;
        }
    }
}

// ref: FUN_14068c520
void GxuFontRegisterCallbacks() {
    GxAddDeviceCreatedCallback(NoDeviceCallback, nullptr);
    GxAddDeviceDestroyedCallback(NoDeviceCallback, nullptr);
}

// ref: FUN_14068b740
void GxuFontSetColorNameLookup(GXUCOLORNAMELOOKUP lookup) {
    s_colorNameLookup = lookup;
}

// ref: FUN_14068ac10
GXUCOLORNAMELOOKUP GxuFontGetColorNameLookup() {
    return s_colorNameLookup;
}

// ref: FUN_14068b7b0
void GxuFontSetStringRenderer(void* renderer) {
    s_stringRenderer = renderer;
}

// ref: FUN_14068b240
void* GxuFontGetStringRenderer() {
    return s_stringRenderer;
}

// ref: FUN_14068ac60
float GxuFontGetIndent() {
    return g_gxuIndent;
}

// The control code at the text, if any: the codes |c, |r, |n, |H, |h, |T, |t, |A, |a, |K,
// |k, |W, |w and ||, unless the flags turn them off; a newline is code 3.
// ref: FUN_140689b30
int32_t GxuDetermineControlCode(const char* text, int32_t* advance, uint32_t* color, uint32_t flags, uint32_t* code) {
    uint32_t noCodes = flags & 0x800;
    uint32_t character = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(text), advance);
    *code = character;
    int32_t length = *advance;
    if (0xfffffffd < character - 1) {
        return 0;
    }
    if (character == 0xd) {
        uint32_t next = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(text + 1), advance);
        *advance = (next == 10) + 1;
        return 3;
    }
    if (character == 10) {
        *advance = 1;
        return 3;
    }
    if (character != 0x7c || noCodes != 0) {
        return 0;
    }
    const char* p = text + length;
    if (*p != '\0') {
        switch (*p) {
        case 'A':
        case 'T':
            if ((flags & 0x1000) == 0) {
                char letter = *p;
                uint32_t terminator = 0x74;
                if (letter != 'A' || (terminator = 0x61, p[1] == ':')) {
                    const char* q = p + length;
                    while (*q != '\0') {
                        int32_t step = 0;
                        uint32_t c = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &step);
                        q += step;
                        if (c == 0x7c) {
                            if (*q != '\0') {
                                int32_t after = 0;
                                uint32_t c2 = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &after);
                                if (c2 == terminator
                                    && static_cast<uint64_t>((q + after) - text) != static_cast<uint64_t>(letter == 'A') + 4) {
                                    *advance = static_cast<int32_t>((q + after) - text);
                                    return letter == 'A' ? 13 : 7;
                                }
                            }
                            break;
                        }
                    }
                }
            }
            break;
        case 'C':
        case 'c':
            if ((flags & 0x100) == 0) {
                if (p[1] == 'N' || p[1] == 'n') {
                    const char* name = p + 2;
                    const char* colon = std::strchr(name, ':');
                    if (colon != nullptr && s_colorNameLookup != nullptr) {
                        if (color != nullptr) {
                            size_t nameLength = static_cast<size_t>(colon - name);
                            char buffer[0x40];
                            char* key = nameLength < sizeof(buffer) ? buffer
                                                                    : static_cast<char*>(SMemAlloc(nameLength + 1, 0, "D", 0, -6, 0));
                            std::memcpy(key, name, nameLength);
                            key[nameLength] = '\0';
                            bool found = false;
                            const uint8_t* rgb = s_colorNameLookup(key, &found);
                            if (key != buffer) {
                                SMemFree(key, "D", -6, 0);
                            }
                            if (!found) {
                                *advance = length;
                                *code = 0x7c;
                                return 0;
                            }
                            std::memcpy(color, rgb, 4);
                            reinterpret_cast<uint8_t*>(color)[3] = 0xff;
                        }
                        *advance = static_cast<int32_t>(colon - text) + 1;
                        return 1;
                    }
                } else {
                    uint8_t bytes[4] = {};
                    uint32_t count = 0;
                    for (uint32_t i = 0;; i += 2) {
                        char a = p[i + 1];
                        char b = a != '\0' ? p[i + 2] : '\0';
                        if (a == '\0' || b == '\0' || a == '\n' || b == '\n' || a == '\r' || b == '\r') {
                            break;
                        }
                        char pair[3] = { a, b, '\0' };
                        char* end = nullptr;
                        long value = strtol(pair, &end, 0x10);
                        if (end != nullptr && *end != '\0') {
                            break;
                        }
                        bytes[count++] = static_cast<uint8_t>(value);
                        if (3 < count) {
                            if (color != nullptr) {
                                *color = 0xff000000u | (static_cast<uint32_t>(bytes[1]) << 16)
                                    | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
                            }
                            *advance = 10;
                            return 1;
                        }
                    }
                }
            }
            break;
        case 'H':
            if ((flags & 0x400) == 0) {
                const char* q = p + length;
                while (*q != '\0') {
                    int32_t step = 0;
                    uint32_t c = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &step);
                    q += step;
                    if (c != 0x7c) {
                        continue;
                    }
                    if (*q == '\0') {
                        break;
                    }
                    uint32_t c2 = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &step);
                    q += step;
                    if (c2 != 0x4b) {
                        if (*q != '\0' && c2 == 0x68 && (q - text) != 4 && (*q != '|' || q[1] != 'h')) {
                            *advance = static_cast<int32_t>(q - text);
                            return 5;
                        }
                        break;
                    }
                    if (*q == '\0') {
                        break;
                    }
                    const char* r = q + 1;
                    bool valid = false;
                    for (;;) {
                        if (*r == '\0') {
                            goto fail;
                        }
                        int32_t rstep = 0;
                        uint32_t d = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(r), &rstep);
                        r += rstep;
                        if (d == 0x7c) {
                            valid = true;
                            break;
                        }
                        if (9 < d - 0x30) {
                            goto fail;
                        }
                    }
                    if (!valid || *r == '\0') {
                        break;
                    }
                    int32_t kstep = 0;
                    uint32_t k = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(r), &kstep);
                    q = r + kstep;
                    if (k != 0x6b) {
                        break;
                    }
                }
            }
            break;
        case 'K': {
            const char* q = p + length + 1;
            if (p[length] != '\0' && *q != '\0') {
                int32_t step = 0;
                for (;;) {
                    uint32_t d = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &step);
                    q += step;
                    if (d == 0x7c) {
                        break;
                    }
                    if (9 < d - 0x30 || *q == '\0') {
                        goto fail;
                    }
                }
                if (*q != '\0') {
                    uint32_t k = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(q), &step);
                    if (k == 0x6b) {
                        *advance = static_cast<int32_t>((q + step) - text) - 2;
                        return 9;
                    }
                }
            }
            break;
        }
        case 'N':
        case 'n':
            if ((flags & 0x200) == 0) {
                *advance = 2;
                return 3;
            }
            break;
        case 'R':
        case 'r':
            if ((flags & 0x100) == 0) {
                *advance = 2;
                return 2;
            }
            break;
        case 'W':
            *advance = 2;
            return 0xb;
        case 'a':
            if ((flags & 0x1000) == 0) {
                *advance = 2;
                return 0xe;
            }
            break;
        case 'h':
            if ((flags & 0x400) == 0) {
                *advance = 2;
                return 6;
            }
            break;
        case 'k':
            *advance = 2;
            return 10;
        case 't':
            if ((flags & 0x1000) == 0) {
                *advance = 2;
                return 8;
            }
            break;
        case 'w':
            *advance = 2;
            return 0xc;
        case '|':
            *advance = 2;
            return 4;
        }
    }
fail:
    *advance = length;
    *code = 0x7c;
    return 0;
}

// Steps back from *position: over a |T...|t or |A...|a code ending there, or to the start
// of the UTF-8 character.
// ref: FUN_14068a210
void GxuFontBackUpOverCode(const char* text, uint32_t* position) {
    uint32_t index = *position;
    bool inCode = false;
    char letter = 'A';
    for (;;) {
        if (index == 0) {
            return;
        }
        char c = text[index];
        bool done;
        uint32_t previous;
        if ((c == 'a' || c == 't') && text[index - 1] == '|') {
            inCode = true;
            letter = c == 'a' ? 'A' : 'T';
            *position = index - 1;
        check:
            previous = index - 1;
            if (text[index - 1] != '|' || text[index] != letter) {
                goto other;
            }
            done = true;
        } else {
            *position = index - 1;
            if (inCode) {
                goto check;
            }
        other:
            done = false;
            previous = index - 1;
            if (!inCode && (static_cast<uint8_t>(text[previous]) & 0xc0) != 0x80) {
                return;
            }
        }
        if (done) {
            return;
        }
        index = previous;
    }
}

// Copies at most `outSize` - 1 bytes of text, dropping the codes whose flag is in `mask`.
// ref: FUN_14068ba40
char* GxuFontStripCodes(const char* text, int32_t length, uint32_t mask, char* out, uint32_t outSize) {
    *out = '\0';
    if (text == nullptr || *text == '\0' || length == 0) {
        const char* source = text;
        for (uint64_t room = outSize; room != 0;) {
            room--;
            if (room == 0) {
                *out = '\0';
                return const_cast<char*>(text);
            }
            char c = *source++;
            *out++ = c;
            if (c == '\0') {
                break;
            }
        }
        return const_cast<char*>(text);
    }
    uint32_t written = 0;
    const char* cursor = text;
    do {
        if (length == 0) {
            break;
        }
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(cursor, &advance, nullptr, 0, &code);
        switch (kind) {
        case 1: case 2: case 5: case 7:
            if ((s_stripCodes[kind].m_flag & mask) != 0) {
                cursor += advance;
                length -= advance;
                continue;
            }
            if (outSize - static_cast<uint32_t>(advance) <= written) {
                goto finish;
            }
            for (int32_t i = 0; i < advance; i++) {
                out[written++] = *cursor++;
            }
            length -= advance;
            continue;
        case 3: case 4: case 6: case 8:
            length -= advance;
            if ((s_stripCodes[kind].m_flag & mask) == 0) {
                if (outSize - static_cast<uint32_t>(advance) <= written) {
                    goto finish;
                }
                if (s_stripCodes[kind].m_pipe != 0) {
                    out[written++] = '|';
                }
                out[written++] = s_stripCodes[kind].m_letter;
            }
            cursor += advance;
            continue;
        default:
            if (outSize - static_cast<uint32_t>(advance) <= written) {
                goto finish;
            }
            for (int32_t i = 0; i < advance; i++) {
                out[written++] = *cursor++;
            }
            length -= advance;
            continue;
        }
    } while (*cursor != '\0');
finish:
    out[written] = '\0';
    return out;
}

// ---- fonts -----------------------------------------------------------------------------------

namespace {

template <class Init>
bool CreateFont(CGxuFont** out, uint32_t flags, Init init) {
    void* memory = SMemAlloc(sizeof(CGxuFont), 0x1b, s_mainFile, SStrHashFnv1a(s_mainFile), 0x191, 0);
    auto font = new (memory) CGxuFont();
    font->m_link.LinkToTail(&g_gxuFonts);
    uint32_t fontFlags = flags;
    if ((flags & 8) != 0) {
        fontFlags |= 1;
    }
    bool loaded = init(font, fontFlags);
    if (!loaded) {
        font->~CGxuFont();
        SMemFree(font, s_mainFile, -2, 0);
        *out = nullptr;
        return false;
    }
    *out = font;
    return true;
}

}

// ref: FUN_14068a5c0
bool GxuFontCreateFont(int32_t fileDataId, float height, CGxuFont** font, uint32_t flags) {
    return CreateFont(font, flags, [fileDataId, height](CGxuFont* made, uint32_t fontFlags) {
        return made->InitializeById(fileDataId, fontFlags, height);
    });
}

// ref: FUN_14068a6d0
bool GxuFontCreateFontByName(const char* path, float height, CGxuFont** font, uint32_t flags) {
    return CreateFont(font, flags, [path, height](CGxuFont* made, uint32_t fontFlags) {
        return made->InitializeByName(path, fontFlags, height);
    });
}

// ref: FUN_14068ab80
void GxuFontDestroyFont(CGxuFont** font) {
    CGxuFont* destroyed = *font;
    if (destroyed != nullptr) {
        destroyed->~CGxuFont();
        SMemFree(destroyed, s_mainFile, -2, 0);
    }
    *font = nullptr;
}

// ref: FUN_14068ac20
int32_t GxuFontGetFileDataId(CGxuFont* font) {
    return font != nullptr ? font->m_fileDataId : 0;
}

// ref: FUN_14068ac30
uint32_t GxuFontGetFlags(CGxuFont* font) {
    return font != nullptr ? font->m_flags : 0;
}

// ref: FUN_14068b1b0
uint32_t GxuFontGetPixelSize(CGxuFont* font) {
    return font != nullptr ? font->m_pixelSize : 0;
}

// ref: FUN_14068b750
void GxuFontSetFlag40(CGxuFont* font) {
    if (font != nullptr) {
        font->m_flags |= 0x40;
    }
}

// ref: FUN_14068ac70
float GxuFontGetEffectiveHeight(CGxuFont* font, float height, uint32_t flags) {
    if ((flags & 4) != 0) {
        return static_cast<float>(font->GetPixelSize()) / static_cast<float>(g_gxuScreenHeight);
    }
    return height;
}

// ref: FUN_14068a2d0
GXUFONTPIXELSIZE GxuFontGetFacePixelSize(uint32_t fileDataId, float height, uint32_t flags) {
    FT_FaceRec_* face = GxuFontFaceFindById(fileDataId);
    if (face == nullptr) {
        return {2, 0};
    }
    return {1, GxuFontComputePixelSize(face, height, flags)};
}

// ref: FUN_14068a320
GXUFONTPIXELSIZE GxuFontGetFacePixelSizeByName(const char* path, float height, uint32_t flags) {
    FT_FaceRec_* face = GxuFontFaceFindByName(path);
    if (face == nullptr) {
        return {2, 0};
    }
    return {1, GxuFontComputePixelSize(face, height, flags)};
}

// ref: FUN_14068b630
void* GxuFontCreateFaceFromBuffer(const char* path, void* data, uint32_t size) {
    if (g_gxuFreeTypeLibrary == nullptr) {
        return nullptr;
    }
    return GxuFontFaceCreateFromBuffer(path, g_gxuFreeTypeLibrary, data, size);
}

// ---- measuring -------------------------------------------------------------------------------

// ref: FUN_14068acb0
int32_t GxuFontGetTextExtent(CGxuFont* font, const char* text, float height, float width, uint32_t length,
    float* extent, float indent, float scale, float unused, uint32_t flags) {
    (void)unused;
    return GxuFontGetTextExtentWithPositions(font, text, height, width, length, extent, flags, indent, scale, nullptr,
        nullptr);
}

// ref: FUN_14068b210
void GxuFontGetWrappedWidth(CGxuFont* font, const char* text, uint32_t length, float height, float* extent,
    float indent, float scale, float unused, uint32_t flags) {
    (void)unused;
    GxuFontGetWrappedExtent(font, text, length, height, extent, flags, indent, scale);
}

// ref: FUN_14068b250
float GxuFontGetWrappedTextHeight(CGxuFont* font, const char* text, float height, float width, const float* indent,
    float scale, float spacing, uint32_t flags) {
    if ((flags & 4) != 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    uint32_t lines = 0;
    float extra = 0.0f;
    uint8_t wrapped = 1;
    const char* next = nullptr;
    float textureHeight = 0.0f;
    if (text == nullptr) {
        return 0.0f;
    }
    while (*text != '\0') {
        if (((flags >> 0xd) & 1) != 0 && lines == 1) {
            width -= g_gxuIndent;
        }
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(text, &advance, nullptr, flags, &code);
        if (kind == 3) {
            next = text + advance;
            textureHeight = 0.0f;
        } else {
            int32_t length = 0;
            float extent = 0.0f;
            GxuFontGetLineExtent(font, text, height, width, &length, &extent, &next, *indent, flags, &wrapped,
                &textureHeight, scale);
            if (text == next) {
                break;
            }
        }
        lines++;
        if (static_cast<float>(g_gxuScreenHeight) * height < textureHeight) {
            extra += textureHeight - static_cast<float>(g_gxuScreenHeight) * height;
        }
        if ((flags & 2) != 0 || (text = next, next == nullptr)) {
            break;
        }
    }
    if (lines == 0) {
        return 0.0f;
    }
    float gap = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(
                    static_cast<float>(g_gxuScreenHeight) * spacing + 0.99995f)))
        / static_cast<float>(g_gxuScreenHeight);
    return static_cast<float>(lines - 1) * gap + static_cast<float>(lines) * height
        + extra / static_cast<float>(g_gxuScreenHeight);
}

// ref: FUN_14068ad10
uint32_t GxuFontGetMaxBytesWithinHeight(CGxuFont* font, const char* text, float height, float width, float maxHeight,
    int32_t lines, float indent, float scale, float spacing, uint32_t flags) {
    if (font == nullptr || text == nullptr || *text == '\0' || height == 0.0f || width == 0.0f || maxHeight == 0.0f
        || lines == 0 || g_gxuScreenHeight == 0) {
        return 0;
    }
    if ((flags & 4) != 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    float used = 0.0f;
    uint32_t count = 0;
    uint32_t total = 0;
    uint8_t wrapped = 1;
    float gap = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(
        static_cast<float>(g_gxuScreenHeight) * spacing + 0.99995f)));
    uint64_t screenHeight = g_gxuScreenHeight;
    while (*text != '\0') {
        if (((flags >> 0xd) & 1) != 0 && count == 1) {
            width -= g_gxuIndent;
        }
        int32_t length = 0;
        float extent = 0.0f;
        const char* next = nullptr;
        GxuFontGetLineExtent(font, text, height, width, &length, &extent, &next, indent, flags, &wrapped, nullptr, scale);
        int32_t taken = static_cast<int32_t>(next - text);
        if (taken == 0) {
            return total;
        }
        count++;
        if (1.1920929e-07f * 8.0f * static_cast<float>(count) + maxHeight < used + height) {
            return total;
        }
        total += static_cast<uint32_t>(taken);
        used += height + gap / static_cast<float>(screenHeight);
        if (wrapped == 0) {
            return total;
        }
        text = next;
        if (next == nullptr) {
            return total;
        }
    }
    return total;
}

// ref: FUN_14068afc0
uint32_t GxuFontGetMaxCharsWithinWidth(CGxuFont* font, const char* text, float height, float width, uint32_t count,
    float* extent, float indent, float scale, float unused, uint32_t flags) {
    (void)unused;
    if (text == nullptr || *text == '\0' || count == 0 || width == 0.0f) {
        return 0;
    }
    auto positions = static_cast<float*>(SMemAlloc(static_cast<uint64_t>(count) * 4, 0, s_mainFile, 0, -2, 0));
    float total = 0.0f;
    int32_t consumed = 0;
    uint32_t characters = static_cast<uint32_t>(GxuFontGetTextExtentWithPositions(font, text, height, 10000.0f, count,
        &total, flags, indent, scale, &consumed, positions));
    uint32_t result;
    if (width < total) {
        uint32_t index = 0;
        result = 0;
        bool found = false;
        for (; index < characters; index++) {
            if (total - width < positions[index]) {
                if (extent != nullptr) {
                    *extent = total - positions[index];
                }
                result = characters - index;
                found = true;
                break;
            }
        }
        if (!found && extent != nullptr) {
            *extent = 0.0f;
        }
    } else {
        if (extent != nullptr) {
            *extent = total;
        }
        result = characters;
    }
    SMemFree(positions, s_mainFile, -2, 0);
    return result;
}

// ref: FUN_14068bda0
uint32_t GxuFontGetLineStarts(CGxuFont* font, const char* text, uint32_t length, float height, float width,
    int32_t* offsets, uint32_t maxOffsets, float indent, float scale, float unused, uint32_t flags) {
    (void)unused;
    if (text == nullptr || *text == '\0' || length == 0 || height == 0.0f || width == 0.0f) {
        return 0;
    }
    if ((flags & 4) != 0) {
        height = GxuFontGetOneToOneHeight(font);
    }
    uint32_t count = 0;
    uint8_t wrapped = 1;
    const char* cursor = text;
    do {
        if (text + length <= cursor) {
            break;
        }
        if (((flags >> 0xd) & 1) != 0 && count == 1) {
            width -= g_gxuIndent;
        }
        if (count < maxOffsets) {
            offsets[count] = static_cast<int32_t>(cursor - text);
        }
        count++;
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(cursor, &advance, nullptr, flags, &code);
        if (kind == 3) {
            cursor += advance;
        } else {
            int32_t lineLength = 0;
            float extent = 0.0f;
            const char* next = nullptr;
            GxuFontGetLineExtent(font, cursor, height, width, &lineLength, &extent, &next, indent, flags, &wrapped,
                nullptr, scale);
            bool same = cursor == next;
            cursor = next;
            if (same) {
                break;
            }
        }
    } while (cursor != nullptr);
    if (text[length - 1] == '\n') {
        if (count < maxOffsets) {
            offsets[count] = static_cast<int32_t>(length);
        }
        count++;
    }
    return count;
}

// ---- strings ---------------------------------------------------------------------------------

// ref: FUN_14068a7e0
bool GxuFontCreateString(CGxuFont* font, const char* text, float height, const float* position, float width,
    float maxHeight, float spacing, CGxString** string, int32_t justifyV, int32_t justifyH, uint32_t flags,
    const uint32_t* color, float unused, float textScale, float scale, float rotation) {
    (void)unused;
    void* memory = SMemAlloc(sizeof(CGxString), 0x1b, s_mainFile, SStrHashFnv1a(s_mainFile), 0x254, 0);
    auto made = new (memory) CGxString();
    made->m_link.LinkToTail(&g_gxuStrings);
    bool initialized = made->Initialize(height, position, width, maxHeight, font, text, justifyV, justifyH, spacing,
        flags & ~1u, color, textScale, scale, rotation);
    if (!initialized) {
        made->~CGxString();
        SMemFree(made, s_mainFile, -2, 0);
        *string = nullptr;
        return false;
    }
    *string = made;
    return true;
}

// ref: FUN_14068abd0
void GxuFontDestroyString(CGxString** string) {
    CGxString* destroyed = *string;
    if (destroyed != nullptr) {
        destroyed->~CGxString();
        SMemFree(destroyed, s_mainFile, -2, 0);
        *string = nullptr;
    }
}

// ref: FUN_14068a1c0
void GxuFontStringSetShadow(CGxString* string, const uint32_t* color, const float* offset) {
    if (string != nullptr) {
        string->SetShadow(offset, color);
    }
}

// ref: FUN_14068b690
void GxuFontStringClearShadow(CGxString* string) {
    if (string != nullptr) {
        string->m_flags &= ~1u;
    }
}

// ref: FUN_14068b780
bool GxuFontStringSetColor(CGxString* string, const uint32_t* color) {
    string->SetColor(color);
    return true;
}

// ref: FUN_14068b770
void GxuFontStringSetScale(CGxString* string, float scale) {
    string->m_scale = scale;
}

// ref: FUN_14068b760
void GxuFontStringSetRotation(CGxString* string, float rotation) {
    string->m_rotation = rotation;
}

// ref: FUN_14068ba30
int32_t GxuFontStringSetGradient(CGxString* string, int32_t start, int32_t length) {
    if (string == nullptr) {
        return -1;
    }
    return string->SetGradient(start, length);
}

// ref: FUN_14068b1c0
void GxuFontStringGetPosition(float* position, CGxString* string) {
    if (string == nullptr) {
        position[0] = 0.0f;
        position[1] = 0.0f;
        position[2] = 0.0f;
        return;
    }
    position[0] = string->m_position[0];
    position[1] = string->m_position[1];
    position[2] = string->m_position[2];
}

// ref: FUN_14068ac40
const char* GxuFontGetFontName(CGxuFont* font) {
    return font != nullptr ? font->m_path : nullptr;
}

// ref: FUN_14068b9c0
int32_t GxuFontStringGetHyperlinks(CGxString* string, GXUHYPERLINKINFO** links) {
    if (string == nullptr) {
        return 0;
    }
    return string->GetHyperlinks(links);
}

// ref: FUN_14068b9b0
int32_t GxuFontStringGetTextures(CGxString* string, GXUEMBEDDEDTEXTURE** textures) {
    if (string == nullptr) {
        return 0;
    }
    return string->GetTextures(textures);
}

// ref: FUN_14068b650
bool GxuFontStringRemoveFromBatch(CGxString* string) {
    if (string == nullptr) {
        return false;
    }
    string->m_batchLink.Unlink();
    return true;
}

// ---- batches ---------------------------------------------------------------------------------

namespace {

GxuStringBatch* NewBatch() {
    void* memory = SMemAlloc(sizeof(GxuStringBatch), 0x1b, s_mainFile, SStrHashFnv1a(s_mainFile), 0x28a, 0);
    auto batch = new (memory) GxuStringBatch();
    batch->m_flags = 0;
    return batch;
}

}

// ref: FUN_14068a390
GxuStringBatch* GxuStringBatchCreate() {
    return NewBatch();
}

// ref: FUN_14068a480
GxuStringBatch* GxuStringBatchCreateWithFlags(bool world, bool a2, bool a3, bool a4) {
    GxuStringBatch* batch = NewBatch();
    uint32_t flags = world ? 1 : 0;
    if (a3) {
        flags |= 2;
    }
    if (a2) {
        flags |= 4;
    }
    if (a4) {
        flags |= 8;
    }
    batch->m_flags = flags;
    return batch;
}

// ref: FUN_14068ab40
bool GxuStringBatchDestroy(GxuStringBatch* batch) {
    if (batch == nullptr) {
        return false;
    }
    batch->Clear();
    batch->~GxuStringBatch();
    SMemFree(batch, s_mainFile, -2, 0);
    return true;
}

// ref: FUN_14068a370
bool GxuStringBatchClear(GxuStringBatch* batch) {
    if (batch == nullptr) {
        return false;
    }
    batch->Clear();
    return true;
}

// ref: FUN_14068a1e0
bool GxuStringBatchAddString(GxuStringBatch* batch, CGxString* string) {
    if (batch == nullptr || string == nullptr) {
        return false;
    }
    batch->AddString(string);
    return true;
}

// ref: FUN_14068b6c0
void GxuStringBatchRender(GxRenderContext* context, GxuStringBatch* batch, const float* view, const float* projection,
    const float* depth) {
    GXUBATCHRENDER params;
    params.m_clip = depth;
    // FUN_14228f6c0: whether the context's first target is multisampled.
    auto target = context->Field<uint8_t*>(0xc8);
    params.m_multisample = target != nullptr ? target[8] : 0;
    params.m_mask = nullptr;
    if (batch != nullptr) {
        batch->Render(context, view, projection, &params);
    }
}
