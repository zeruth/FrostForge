#ifndef TEXTURE_TEXTURE_HPP
#define TEXTURE_TEXTURE_HPP

// Engine/Source/Services/Texture.cpp: textures by FileDataID or path, loaded through the async
// reader, decoded (BLP, TGA, PNG, JPEG) on its thread and uploaded to a Gx texture on the main
// thread; solid-colour textures, unique (caller-filled) textures, the 64-slot atlas pages small
// textures can share, and the reload when the device is recreated or the settings change.

#include <cstdint>

#include "base/Status.hpp"
#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "images/Images.hpp"

struct CAsyncObject;
class CTexture;

// What a Gx texture is created from (0x30 bytes).
struct GXTEXDESC {
    uint8_t m_type;                 // 0x00: 0 2D, 1 cube, 2 2D of any size
    uint8_t m_pad01[3];
    uint32_t m_width;               // 0x04
    uint32_t m_height;              // 0x08
    uint32_t m_depth;               // 0x0c
    uint8_t m_format;               // 0x10: GXTEXFMT_*
    uint8_t m_dataFormat;           // 0x11: the format of the data handed to it
    uint8_t m_pad12[2];
    uint32_t m_usage;               // 0x14: bits 0-2 filter, 13-17 anisotropy, 19 sampled, 28-31 levels
    uint32_t m_usage2;              // 0x18
    uint32_t m_pad1c;
    void* m_heap;                   // 0x20
    uint64_t m_28;                  // 0x28
};
static_assert(sizeof(GXTEXDESC) == 0x30, "GXTEXDESC is 0x30 bytes");

// The texture cache's key (TextureFileHash): the Gx flags and the file data id, or with
// m_matchAny any flags.
struct TEXTUREKEY {
    uint64_t m_flags;               // 0x00
    int32_t m_fileDataId;           // 0x08
    uint8_t m_matchAny;             // 0x0c
};

// A page of the texture atlas (0x248 bytes): 8x8 slots of one size and format.
struct CTextureAtlas {
    int32_t m_refCount;             // 0x00
    void* m_next;                   // 0x08: the page list (TSLink)
    void* m_prev;                   // 0x10
    int32_t m_slotWidth;            // 0x18
    int32_t m_slotHeight;           // 0x1c
    int32_t m_capacity;             // 0x20: 64
    int32_t m_used;                 // 0x24
    int32_t m_freeSlot;             // 0x28: the last freed slot, or -1
    int32_t m_pad2c;
    void* m_gxTex;                  // 0x30
    uint8_t m_format;               // 0x38
    uint8_t m_pad39[3];
    uint32_t m_usage;               // 0x3c: the Gx flags, a uint64 at an unaligned offset
    uint32_t m_usage2;              // 0x40
    uint8_t m_dataFormat;           // 0x44
    uint8_t m_pad45[3];
    CTexture* m_slots[0x40];        // 0x48
};
static_assert(sizeof(CTextureAtlas) == 0x248, "CTextureAtlas is 0x248 bytes");

// The texture object (0x118 bytes, vtable 0x142725fc0).
class CTexture {
    public:
    CTexture();                     // FUN_14127a680
    virtual ~CTexture();            // FUN_14127a7b0 (deleting: FUN_14127b3a0)

    int32_t m_refCount;             // 0x08
    uint32_t m_flags;               // 0x0c: 1 opaque, 2 loose files allowed, 4 atlas, bits 3-5 the
                                    //       file kind, 0x80 reloading, 0x100 any size, 0x200,
                                    //       0x400 keep the placeholder, 0x800 failed, 0x1000 unique,
                                    //       0x2000 the Gx texture failed
    uint8_t m_mipBias;              // 0x10
    uint8_t m_quality;              // 0x11
    uint8_t m_mipCount;             // 0x12
    uint8_t m_alphaDepth;           // 0x13
    int32_t m_compression;          // 0x14
    CStatus m_status;               // 0x18
    CAsyncObject* m_async;          // 0x40
    volatile long m_state;          // 0x48: 1 loading, 2 created, 3 failed
    void* m_gxTex;                  // 0x50
    void* m_placeholder;            // 0x58
    GXTEXDESC m_desc;               // 0x60
    uint64_t m_90;                  // 0x90
    MIPBITS* m_mipBits;             // 0x98
    blz::function_storage m_fill;   // 0xa0: a unique texture's writer
    CTextureAtlas* m_atlas;         // 0xc0
    int32_t m_atlasSlot;            // 0xc8
    uint32_t m_padcc;
    blz::string m_name;             // 0xd0
    int32_t m_fileDataId;           // 0xf8
    uint32_t m_padfc;
    TEXTUREKEY m_key;               // 0x100
    uint8_t m_cache;                // 0x110: 1 by file, 2 unique, 4 solid, 8 in the fill queue,
                                    //        0x10 the global filter applies, 0x20 sampled once loaded
};

// Create flags (TextureCreate's createFlags).
enum : uint32_t {
    TEXTURE_OWN_FILTER = 0x1,       // the caller's filter flags, not the global ones
    TEXTURE_LOOSE_FILES = 0x2,
    TEXTURE_ATLAS = 0x4,
    TEXTURE_ANY_SIZE = 0x20,
    TEXTURE_200 = 0x40,
    TEXTURE_KEEP_PLACEHOLDER = 0x80,
    TEXTURE_NOT_SAMPLED = 0x100,
};

typedef CTexture* HTEXTURE;

// Startup and device events.
void TextureInitialize();
void TextureShutdown();
void TextureOnDeviceCreated(void* param);
void TextureOnDeviceDestroyed(void* param);
void TextureApplySettings();

HTEXTURE TextureCreate(int32_t fileDataId, uint64_t gxFlags, CStatus* status, uint32_t createFlags, uint32_t priority,
    uint8_t quality);
HTEXTURE TextureCreateByName(const char* name, uint64_t gxFlags, CStatus* status, uint32_t createFlags,
    uint32_t priority, uint8_t quality);
HTEXTURE TextureCreateById(int32_t fileDataId, CStatus* status);
HTEXTURE TextureCreateByNameDefault(const char* name, CStatus* status);
HTEXTURE TextureCreateSolid(const uint32_t* color, const uint32_t* color2, const char* name, uint8_t kind);
HTEXTURE TextureCreateSolidColor(const uint32_t* color, const char* name);
HTEXTURE TextureGetWhite();
HTEXTURE TextureCreateUnique(uint8_t type, uint32_t width, uint32_t height, uint32_t depth, uint8_t format,
    uint8_t dataFormat, uint64_t gxFlags, blz::function_storage* fill, const char* name, bool ownFilter);
void TextureAddRef(HTEXTURE texture);
int32_t TextureGetRefCount(HTEXTURE texture);
int32_t TextureGetMemorySize(HTEXTURE texture);
void TextureRelease(HTEXTURE texture, int32_t flag);
void TextureFree(HTEXTURE* texture);

// The Gx texture to draw with: the texture's own once loaded, else its placeholder, else the
// default (solid) texture. The blocking forms wait for the load.
void* TextureGetGxTex(HTEXTURE texture);
void* TextureGetGxTexOrDefault(HTEXTURE texture);
void* TextureGetGxTexOrDefaultRecreate(HTEXTURE texture);
void* TextureGetGxTexBlocking(HTEXTURE texture);
void* TextureGetGxTexIfResident(HTEXTURE texture);
void* TextureGetGxTexWait(HTEXTURE texture);
void* TextureGetGxTexPreferPlaceholder(HTEXTURE texture);
void* TextureDefaultGxTex();
void* TextureBlackGxTex();
void* TextureGreyGxTex();
void* TextureWhiteGxTex();
void* TextureTransparentGreyGxTex();
void* TextureTransparentBlack3DGxTex();

bool TextureGetSize(HTEXTURE texture, uint32_t* width, uint32_t* height, bool wait);
int32_t TextureGetFileDataId(HTEXTURE texture);
char* TextureGetName(HTEXTURE texture, char* buffer, uint64_t size);
bool TextureIsLoaded(HTEXTURE texture);
bool TextureIsLoading(HTEXTURE texture);
bool TextureIsReady(HTEXTURE texture);
bool TextureHasPlaceholder(HTEXTURE texture);
bool TextureIsOpaque(HTEXTURE texture);
bool TextureInFileCache(HTEXTURE texture);
bool TextureHasAtlas(HTEXTURE texture);
bool TextureGetAtlasCoords(HTEXTURE texture, float* origin, float* scale);
int32_t TextureLoadState(HTEXTURE texture);
bool TextureSetPriority(HTEXTURE texture, int32_t priority);
bool TextureIsSampleable(HTEXTURE texture);

// Callbacks: per texture as it finishes loading (given its file data id), and once a frame after
// any finished.
void TextureAddLoadedCallback(blz::function_storage* callback, uint64_t key);
void TextureAddAllLoadedCallback(blz::function_storage* callback, uint64_t key);
void TextureRemoveAllLoadedCallback(uint64_t key);

// Global quality.
void TextureSetAtlasEnabled(bool enabled);
void TextureSetResolutionReduction(uint32_t reduction);
uint8_t TextureGetFilter();
uint32_t TextureGetAnisotropy();

// A texture file loaded and decoded at once, outside the cache.
MIPBITS* TextureLoadImageById(int32_t fileDataId, uint32_t* width, uint32_t* height, uint32_t* format, bool* opaque,
    CStatus* status, uint32_t* alphaBits, uint8_t allowLoose);
MIPBITS* TextureLoadImageByName(const char* name, uint32_t* width, uint32_t* height, uint32_t* format, bool* opaque,
    CStatus* status, uint32_t* alphaBits, uint8_t allowLoose);

// Whether a texture is a file (FUN_141280f10), and two files the same (FUN_141280e60): by
// content key when both have one, else by FileDataID.
bool TextureIsSameFile(HTEXTURE texture, int32_t fileDataId);
bool TextureIsSameFileId(int32_t fileDataId, int32_t other);

// The bytes of a Gx texture of a format, filter and size, with its levels.
int32_t TextureMemorySize(uint8_t format, uint32_t filter, uint32_t width, uint32_t height);

#endif
