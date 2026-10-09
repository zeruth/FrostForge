// Gxu fonts (Engine/Source/Gxu/GxuFontFont.cpp): a face at a pixel size with its glyph cache
// across eight page textures, its kerning and the strings laid out with it.

#include <cmath>
#include <cstring>
#include <new>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "blz/ChainedHash.hpp"
#include "gx/Gx.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"

namespace {

const char s_fontFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontFont.cpp";
const char s_miscFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontMiscClasses.cpp";
const char s_codeNodeTag[] = ".?AU?$chained_hash_node@I@blz@@";
const char s_codeBucketTag[] = ".PEAU?$chained_hash_node@I@blz@@";

// The printable pairs the kerning cache holds: 0x20 through 0x7e.
const uint32_t KERN_CACHE_SIZE = 0x2341;

// blz::chained_hash_node<uint32_t>.
struct CODENODE {
    CODENODE* m_next;
    uint32_t m_key;
};

blz::chained_hash_header* CodeSet(GXUCODESET* set) {
    return reinterpret_cast<blz::chained_hash_header*>(set);
}

bool CodeSetHas(GXUCODESET* set, uint32_t code) {
    return set != nullptr && set->m_buckets != nullptr
        && blz::chained_find<CODENODE>(CodeSet(set), code) != nullptr;
}

// Frees a set's nodes and empties its buckets (FUN_140697410's loop, keeping the buckets).
void CodeSetClear(GXUCODESET* set) {
    if (set->m_buckets != nullptr && set->m_bucketCount != 0) {
        for (uint64_t i = 0; i < set->m_bucketCount; i++) {
            auto node = static_cast<CODENODE*>(set->m_buckets[i]);
            while (node != nullptr) {
                CODENODE* next = node->m_next;
                SMemFree(node, s_codeNodeTag, -6, 0);
                node = next;
            }
            set->m_buckets[i] = nullptr;
        }
    }
    set->m_size = 0;
}

// ref: FUN_140697410
void CodeSetDestroy(GXUCODESET* set) {
    if (set->m_buckets != nullptr) {
        for (uint64_t i = 0; i < set->m_bucketCount; i++) {
            auto node = static_cast<CODENODE*>(set->m_buckets[i]);
            while (node != nullptr) {
                CODENODE* next = node->m_next;
                SMemFree(node, s_codeNodeTag, -6, 0);
                node = next;
            }
        }
        SMemFree(set->m_buckets, s_codeBucketTag, -6, 0);
        set->m_buckets = nullptr;
    }
}

// The kerning cache, every pair unknown.
void ResetKernCache(float* cache) {
    for (uint32_t i = 0; i < KERN_CACHE_SIZE; i++) {
        cache[i] = -1.0f;
    }
}

// A kerning pair's hash: FNV-1a over the current character's two bytes, then the previous'.
uint32_t KernHash(uint32_t previous, uint32_t code) {
    uint32_t hash = 0x811c9dc5;
    hash = (hash ^ (code & 0xff)) * 0x1000193;
    hash = (hash ^ ((code & 0xffff) >> 8)) * 0x1000193;
    hash = (hash ^ (previous & 0xff)) * 0x1000193;
    hash = (hash ^ ((previous >> 8) & 0xff)) * 0x1000193;
    return hash;
}

}

// ref: FUN_140688300
CGxuFont::CGxuFont() {
    this->m_link.m_next = nullptr;
    this->m_link.m_prev = nullptr;
    this->m_lastFrame = 0;
    this->m_pad014 = 0;
    this->m_strings.InitSentinel();
    this->m_kerning = nullptr;
    this->m_evicted = nullptr;
    this->m_evictedPrevious = nullptr;
    this->m_kernCache = nullptr;
    this->m_recent.InitEmpty(0x40);
    this->m_face = nullptr;
    this->m_path[0] = '\0';
    this->m_fileDataId = 0;
    this->m_slug = GXUSLUGPTR();
    this->m_cellSize = 0;
    this->m_baseline = 0;
    this->m_flags = 0;
    this->m_requestedHeight = 0.0f;
    this->m_unitScale = 0.0f;
    this->m_pad1e4 = 0;
    for (TEXTURECACHE& page : this->m_pages) {
        std::memset(&page, 0, sizeof(page));
        page.m_layers = 1;
    }
    this->m_pixelSize = 0;
    this->m_pad3ac = 0;
}

// ref: FUN_1406974a0
CGxuFont::~CGxuFont() {
    this->Destroy();
    for (int32_t i = 7; i >= 0; i--) {
        TextureCacheClear(&this->m_pages[i]);
    }
    GxuSlugRelease(&this->m_slug);
    this->m_recent.UnlinkAll();
    this->m_recent.m_terminator.Unlink();
    for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings;) {
        GXULINK* next = link->m_next;
        link->m_next = nullptr;
        link->m_prev = nullptr;
        link = next;
    }
    this->m_strings.m_next = nullptr;
    this->m_strings.m_prev = nullptr;
    this->m_link.Unlink();
}

// ref: FUN_1406992b0
bool CGxuFont::InitializeById(int32_t fileDataId, uint32_t flags, float height) {
    this->m_requestedHeight = height;
    this->m_path[0] = '\0';
    this->m_fileDataId = fileDataId;
    this->Destroy();
    this->m_flags = flags | 0x20;
    if (this->m_face != nullptr) {
        HandleClose(this->m_face);
    }
    this->m_face = GxuFontFaceCreateById(static_cast<uint32_t>(fileDataId), g_gxuFreeTypeLibrary);
    return this->Initialize();
}

// ref: FUN_140699330
bool CGxuFont::InitializeByName(const char* path, uint32_t flags, float height) {
    size_t length = 0;
    for (; length < sizeof(this->m_path) - 1 && path[length] != '\0'; length++) {
        this->m_path[length] = path[length];
    }
    this->m_path[length] = '\0';
    this->m_requestedHeight = height;
    this->m_fileDataId = 0;
    this->Destroy();
    this->m_flags = flags | 0x20;
    if (this->m_face != nullptr) {
        HandleClose(this->m_face);
    }
    this->m_face = GxuFontFaceCreateByName(path, g_gxuFreeTypeLibrary);
    return this->Initialize();
}

// The kerning tables when the face kerns, then the size.
// ref: FUN_140699d40
bool CGxuFont::Initialize() {
    if (this->m_face == nullptr) {
        return false;
    }
    FT_Face face = GxuFontFaceGetFace(this->m_face);
    if ((face->face_flags & FT_FACE_FLAG_KERNING) != 0) {
        this->m_kernCache = static_cast<float*>(SMemAlloc(0x8d04, 0x1b, s_fontFile, SStrHashFnv1a(s_fontFile), 0xed, 0));
        ResetKernCache(this->m_kernCache);
        void* memory = SMemAlloc(sizeof(KERNTABLE), 0x1b, s_fontFile, SStrHashFnv1a(s_fontFile), 0xf0, 0);
        this->m_kerning = new (memory) KERNTABLE();
    }
    if (GxuSlugForceGenerate()) {
        this->SlugData();
    }
    return this->SetSize();
}

// The pixel size from the requested height (at least the face's smallest bitmap size and two
// pixels, rounded to whole pixels, unless exact), the baseline and the cell.
// ref: FUN_14069a2b0
bool CGxuFont::SetSize() {
    FT_Face face = GxuFontFaceGetFace(this->m_face);
    float height = this->m_requestedHeight;
    if ((this->m_flags & 0x10) == 0) {
        if (face->num_fixed_sizes != 0) {
            int32_t smallest = 0x7fffffff;
            for (int32_t i = 0; i < face->num_fixed_sizes; i++) {
                if (face->available_sizes[i].height < smallest) {
                    smallest = face->available_sizes[i].height;
                }
            }
            float fixed = GxuFontPixelHeight(smallest);
            if (height <= fixed) {
                height = fixed;
            }
        }
        float minimum = GxuFontPixelHeight(2);
        if (height <= minimum) {
            height = minimum;
        }
        height = GxuFontRoundHeight(false, height);
    }
    uint32_t pixels = static_cast<uint32_t>(static_cast<int64_t>(height));
    if (0xff < pixels - 1) {
        pixels = ((~(static_cast<int32_t>(pixels - 1) >> 31)) & 0xff) + 1;
    }
    this->m_pixelSize = pixels;
    int32_t descender = face->descender < 0 ? -face->descender : face->descender;
    int32_t extent = descender + face->ascender;
    if (extent == 0) {
        return false;
    }
    this->m_baseline = static_cast<uint32_t>(static_cast<int64_t>(
        static_cast<float>(static_cast<uint32_t>(face->ascender * static_cast<int32_t>(pixels))) / static_cast<float>(extent)
        + 0.5f));
    uint32_t outline = 0;
    if ((this->m_flags & 8) != 0) {
        outline = 4;
    } else if ((this->m_flags & 1) != 0) {
        outline = 2;
    }
    this->m_cellSize = pixels + outline;
    if (0x100 < pixels + outline) {
        this->m_cellSize = 0x100;
        this->m_pixelSize = 0x100 - outline;
        pixels = 0x100 - outline;
    }
    FT_Error error = FT_Set_Pixel_Sizes(face, pixels, 0);
    this->m_unitScale = static_cast<float>(face->size->metrics.x_ppem) / static_cast<float>(face->units_per_EM);
    return error == 0;
}

// The face, glyphs, kerning and Slug data go.
// ref: FUN_140697dc0
void CGxuFont::Destroy() {
    GxuSlugRelease(&this->m_slug);
    if (this->m_face != nullptr) {
        HandleClose(this->m_face);
    }
    this->m_face = nullptr;
    this->ClearGlyphs();
    if (this->m_kernCache != nullptr) {
        SMemFree(this->m_kernCache, s_fontFile, 0x17c, 0);
        this->m_kernCache = nullptr;
    }
    if (this->m_kerning != nullptr) {
        this->m_kerning->~KERNTABLE();
        SMemFree(this->m_kerning, s_fontFile, -2, 0);
        this->m_kerning = nullptr;
    }
    if (this->m_evicted != nullptr) {
        CodeSetDestroy(this->m_evicted);
        SMemFree(this->m_evicted, s_fontFile, -2, 0);
    }
    this->m_evicted = nullptr;
    if (this->m_evictedPrevious != nullptr) {
        CodeSetDestroy(this->m_evictedPrevious);
        SMemFree(this->m_evictedPrevious, s_fontFile, -2, 0);
    }
    this->m_evictedPrevious = nullptr;
}

// Every page and glyph goes (its strings told their pages changed), and the kerning learnt.
// ref: FUN_140697f10
void CGxuFont::ClearGlyphs() {
    for (uint32_t i = 0; i < 8; i++) {
        if (this->m_pages[i].m_texture != nullptr) {
            for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings; link = link->m_next) {
                reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x18)->PageChanged(i);
            }
        }
        TextureCacheClear(&this->m_pages[i]);
    }
    this->m_glyphs.Clear(false);
    while (CharCodeDesc* glyph = this->m_recent.Head()) {
        glyph->m_recentLink.Unlink();
        glyph->m_rowLink.Unlink();
        SMemFree(glyph, ".?AV?$TSList@UCharCodeDesc@@V?$TSGetExplicitLink@UCharCodeDesc@@@@$07@@", -2, 0);
    }
    if (this->m_kerning != nullptr) {
        this->m_kerning->Clear(false);
    }
    if (this->m_kernCache != nullptr) {
        ResetKernCache(this->m_kernCache);
    }
}

// ref: FUN_140698500
void CGxuFont::ReleaseSlug() {
    GxuSlugRelease(&this->m_slug);
}

// The glyphs again at the current screen size, and every string laid out again.
// ref: FUN_1406991f0
void CGxuFont::Rebuild() {
    this->ClearGlyphs();
    this->SetSize();
    for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings; link = link->m_next) {
        reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x18)->Recalculate();
    }
}

// This period's evicted codes become last period's, and a new period starts empty.
// ref: FUN_14069a460
void CGxuFont::SwapEvicted() {
    GXUCODESET* previous = this->m_evictedPrevious;
    this->m_evictedPrevious = this->m_evicted;
    this->m_evicted = previous;
    if (previous != nullptr) {
        CodeSetClear(previous);
    }
}

// A character's glyph, from the cache (made the most recently used) or rendered into a page;
// with every page full the least recently used glyphs make room.
// ref: FUN_140699940
CharCodeDesc* CGxuFont::GetGlyph(uint32_t code) {
    this->m_lastFrame = GxDeviceFrame();
    CharCodeDesc* found = this->m_glyphs.Find(code, [code](CharCodeDesc* glyph) {
        return glyph->m_hashval == code;
    });
    if (found != nullptr) {
        this->m_recent.LinkToHead(found);
        return found;
    }
    GXUGLYPHBITMAP bitmap;
    FT_Face face = GxuFontFaceGetFace(this->m_face);
    FT_Set_Pixel_Sizes(face, this->m_pixelSize, 0);
    int32_t outline = 0;
    if ((this->m_flags & 8) != 0) {
        outline = 4;
    } else if ((this->m_flags & 1) != 0) {
        outline = 2;
    }
    CharCodeDesc* glyph = nullptr;
    TEXTURECACHE* page = nullptr;
    if (IGxuFontGlyphRender(face, this->m_pixelSize, code, static_cast<int32_t>(this->m_baseline), &bitmap,
            ((this->m_flags >> 1) & 1) != 0, outline)) {
        bool placed = false;
        for (uint32_t i = 0; i < 8; i++) {
            TEXTURECACHE* candidate = &this->m_pages[i];
            if (candidate->m_texture == nullptr) {
                this->m_flags &= ~0x20u;
                TextureCacheCreate(candidate, (this->m_flags & 4) != 0, this->m_cellSize);
                TextureCacheSetRows(candidate, this, this->m_cellSize, (~this->m_flags & 1) * 2 + 2);
                glyph = TextureCacheAllocate(candidate, code, &bitmap, this);
                if (glyph != nullptr) {
                    glyph->m_page = i;
                    page = candidate;
                    placed = true;
                }
                break;
            }
            glyph = TextureCacheAllocate(candidate, code, &bitmap, this);
            if (glyph != nullptr) {
                glyph->m_page = i;
                page = candidate;
                placed = true;
                break;
            }
        }
        if (!placed) {
            CharCodeDesc* victim = this->m_recent.m_terminator.m_prevlink->m_prevlink->Next();
            if (victim != nullptr) {
                uint32_t pageIndex = victim->m_page;
                uint32_t rowIndex = victim->m_row;
                page = &this->m_pages[pageIndex];
                uint32_t length = page->m_width;
                TEXTURECACHEROW* row = &page->m_rows[rowIndex];
                TextureCacheRowEvict(row, victim, &bitmap, length, this);
                for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings; link = link->m_next) {
                    reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x18)->PageChanged(pageIndex);
                }
                glyph = TextureCacheRowAllocate(row, code, &bitmap, rowIndex, this->m_cellSize, length,
                    page->m_rowCount / page->m_layers, this);
                if (glyph != nullptr) {
                    glyph->m_row = rowIndex;
                    glyph->m_page = pageIndex;
                    placed = true;
                }
            }
        }
        if (placed) {
            this->m_glyphs.Insert(glyph, code);
            glyph->m_hashval = code;
            this->m_recent.LinkToHead(glyph);
            if (page != nullptr) {
                page->m_dirty = 1;
            }
        }
    }
    SMemFree(bitmap.m_data, s_miscFile, 0x10, 0);
    bitmap.m_data = nullptr;
    return glyph;
}

// A cached glyph's advance, without rendering it.
// ref: FUN_140698930
float CGxuFont::GetAdvance(uint32_t code) {
    CharCodeDesc* glyph = this->m_glyphs.Find(code, [code](CharCodeDesc* found) {
        return found->m_hashval == code;
    });
    if (glyph == nullptr) {
        return 0.0f;
    }
    if ((this->m_flags & 8) == 0) {
        return glyph->m_advance;
    }
    return glyph->m_advance + 1.0f;
}

// The pen step from `previous` to `code`: its advance with the pair's kerning (never wider),
// cached per pair.
// ref: FUN_1406985a0
float CGxuFont::GetKernedAdvance(uint32_t previous, uint32_t code) {
    if (this->m_kernCache == nullptr) {
        return ceilf(this->GetAdvance(previous));
    }
    float* cached = nullptr;
    KernNode* node = nullptr;
    uint32_t hash = 0;
    if (previous - 0x20 < 0x5f && code - 0x20 < 0x5f) {
        cached = &this->m_kernCache[code * 0x5f - 0xc00 + previous];
        if (0.0f <= *cached) {
            return *cached;
        }
    } else {
        hash = KernHash(previous, code);
        uint32_t pair = (code & 0xffff) | (previous << 16);
        node = this->m_kerning->Find(hash, [pair](KernNode* found) {
            return found->m_pair == pair;
        });
        if (node != nullptr) {
            return node->m_step;
        }
    }
    FT_Face face = GxuFontFaceGetFace(this->m_face);
    FT_UInt left = FT_Get_Char_Index(face, previous);
    FT_UInt right = FT_Get_Char_Index(face, code);
    FT_Vector kerning = { 0, 0 };
    FT_Get_Kerning(face, left, right, FT_KERNING_UNSCALED, &kerning);
    int32_t amount = kerning.x < 0 ? static_cast<int32_t>(kerning.x) : 0;
    float step = ceilf(this->GetAdvance(previous) + static_cast<float>(amount) * this->m_unitScale);
    if (cached != nullptr) {
        *cached = step;
    } else {
        KernNode* fresh = this->m_kerning->New(hash, 0, 0);
        fresh->m_hashval = hash;
        fresh->m_pair = (code & 0xffff) | (previous << 16);
        fresh->m_step = step;
    }
    return step;
}

// The monospaced step: three quarters of the cell, less half the slack after a narrow
// character before a wide one.
// ref: FUN_140698860
float CGxuFont::GetMonoAdvance(uint32_t previous, uint32_t code) {
    float before = this->GetAdvance(previous);
    uint32_t cell = this->m_cellSize;
    if (static_cast<uint32_t>(static_cast<int64_t>(before)) < cell) {
        float after = this->GetAdvance(code);
        if (cell <= static_cast<uint32_t>(static_cast<int64_t>(after))) {
            cell -= (cell - static_cast<uint32_t>(static_cast<int64_t>(before))) >> 1;
        }
    }
    return static_cast<float>(cell) * 0.75f;
}

// A glyph's left bearing: whole pixels, or scaled to the height for scalable fonts.
// ref: FUN_1406989b0
float CGxuFont::GetBearing(CharCodeDesc* glyph, bool scaled, float height) {
    if ((this->m_flags & 4) != 0) {
        return (GxuFontRoundHeight(scaled, height) / static_cast<float>(this->m_pixelSize)) * glyph->m_bearing;
    }
    return ceilf(glyph->m_bearing);
}

// Whether every character of a string is cached and was not evicted lately.
// ref: FUN_140697c10
bool CGxuFont::HasGlyphs(const char* text) {
    while (text != nullptr && *text != '\0') {
        int32_t length = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(text), &length);
        text += length;
        if (code == 0xd || code == 10) {
            continue;
        }
        if (CodeSetHas(this->m_evicted, code) || CodeSetHas(this->m_evictedPrevious, code)) {
            return false;
        }
        if (this->m_glyphs.m_slotmod == 0xffffffff) {
            return false;
        }
        CharCodeDesc* glyph = this->m_glyphs.Find(code, [code](CharCodeDesc* found) {
            return found->m_hashval == code;
        });
        if (glyph == nullptr) {
            return false;
        }
    }
    return true;
}

// ref: FUN_140697b00
void CGxuFont::NoteEvicted(uint32_t code) {
    if (this->m_evicted == nullptr) {
        void* memory = SMemAlloc(sizeof(GXUCODESET), 0x1b, s_fontFile, SStrHashFnv1a(s_fontFile), -2, 0);
        this->m_evicted = new (memory) GXUCODESET();
    }
    bool inserted = false;
    blz::chained_insert<CODENODE>(CodeSet(this->m_evicted), code, &inserted, s_codeNodeTag, s_codeBucketTag);
}

// The font's Slug data, loaded the first time it is asked for.
// ref: FUN_140698a50
void* CGxuFont::SlugData() {
    if (this->m_slug.m_ptr != nullptr) {
        return this->m_slug.m_ptr;
    }
    if (this->m_path[0] == '\0') {
        if (this->m_fileDataId == 0) {
            return nullptr;
        }
        GxuSlugLoadById(&this->m_slug, this->m_fileDataId, this->m_flags & 9);
    } else {
        GxuSlugLoadByName(&this->m_slug, this->m_path, this->m_flags & 9);
    }
    return this->m_slug.m_ptr;
}

// ref: FUN_140698a30
void* CGxuFont::SlugGlyphs() {
    auto data = static_cast<uint8_t*>(this->SlugData());
    if (data == nullptr) {
        return nullptr;
    }
    return *reinterpret_cast<void**>(data + 8);
}

// Whether the font draws through Slug: the setting is on, the font is scalable and its Slug
// data and textures exist.
// ref: FUN_140699240
bool CGxuFont::UsesSlug() {
    if (!GxuSlugEnabled() || (this->m_flags & 4) == 0) {
        return false;
    }
    auto data = static_cast<uint8_t*>(this->SlugData());
    if (data == nullptr || *reinterpret_cast<void**>(data + 8) == nullptr) {
        return false;
    }
    void* curves = nullptr;
    void* bands = nullptr;
    GxuSlugGetTextures(data, &curves, &bands);
    return bands != nullptr && curves != nullptr;
}

// ref: FUN_1406993e0
void CGxuFont::PrepareSlug() {
    if (!GxuSlugEnabled() || (this->m_flags & 4) == 0) {
        return;
    }
    auto data = static_cast<uint8_t*>(this->SlugData());
    if (data == nullptr || *reinterpret_cast<void**>(data + 8) == nullptr) {
        return;
    }
    void* curves = nullptr;
    void* bands = nullptr;
    GxuSlugGetTextures(data, &curves, &bands);
}

// The pixel size a face would get at a height (FUN_140697b70).
// ref: FUN_140697b70
uint32_t GxuFontComputePixelSize(FT_FaceRec_* face, float height, uint32_t flags) {
    if ((flags & 0x10) == 0) {
        if (face->num_fixed_sizes != 0) {
            int32_t smallest = 0x7fffffff;
            for (int32_t i = 0; i < face->num_fixed_sizes; i++) {
                if (face->available_sizes[i].height < smallest) {
                    smallest = face->available_sizes[i].height;
                }
            }
            float fixed = GxuFontPixelHeight(smallest);
            if (height <= fixed) {
                height = fixed;
            }
        }
        float minimum = GxuFontPixelHeight(2);
        if (height <= minimum) {
            height = minimum;
        }
        height = GxuFontRoundHeight(false, height);
    }
    uint32_t pixels = static_cast<uint32_t>(static_cast<int64_t>(height));
    if (pixels - 1 < 0x100) {
        return pixels;
    }
    return ((~(static_cast<int32_t>(pixels - 1) >> 31)) & 0xff) + 1;
}
