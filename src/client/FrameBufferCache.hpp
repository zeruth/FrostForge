#ifndef CLIENT_FRAME_BUFFER_CACHE_HPP
#define CLIENT_FRAME_BUFFER_CACHE_HPP

// FrameBufferCache: the render targets passes draw into, kept between frames and handed out
// by size and format. With texture heaps on (render settings flag 0x200) each cache entry owns
// a heap sized for the screen, so targets of one kind alias the same memory.

#include "gx/GxRenderTargets.hpp"
#include <cstddef>
#include <cstdint>

class GxRenderContext;

// One target of a frame buffer: its format and how it is made (0x14 bytes).
struct FRAMEBUFFERTARGET {
    uint8_t m_format;               // 0x00: 0 none
    uint8_t m_pad01[3];
    uint32_t m_samples;             // 0x04: usage bits 21-23
    uint32_t m_08;                  // 0x08: usage bits 24-25
    uint8_t m_renderTarget;         // 0x0c: usage bit 18
    uint8_t m_unordered;            // 0x0d: usage bit 20
    uint8_t m_shaderResource;       // 0x0e: usage bit 19
    uint8_t m_0f;                   // 0x0f: usage bit 27
    uint8_t m_10;                   // 0x10: the second usage word's bit 2
    uint8_t m_pad11[3];
};
static_assert(sizeof(FRAMEBUFFERTARGET) == 0x14, "FRAMEBUFFERTARGET is 0x14 bytes");

// A set of targets and the textures bound to them (0xf0 bytes). Textures given from outside
// (m_targets.m_texs) are kept; the others come from the cache when the buffer is bound.
struct FRAMEBUFFER {
    int32_t m_width;                // 0x00
    int32_t m_height;               // 0x04
    FRAMEBUFFERTARGET m_color[4];   // 0x08
    FRAMEBUFFERTARGET m_depth;      // 0x58
    int32_t m_size[2];              // 0x6c: as bound, clamped to the device
    uint8_t m_colorEntries[4];      // 0x74: the cache entries in use, 0xff none
    uint8_t m_depthEntry;           // 0x78
    uint8_t m_pad79[7];
    GXRENDERTARGETS m_targets;      // 0x80
    uint8_t m_bound;                // 0xe8
    uint8_t m_pade9[7];
};
static_assert(offsetof(FRAMEBUFFER, m_size) == 0x6c, "FRAMEBUFFER layout");
static_assert(offsetof(FRAMEBUFFER, m_targets) == 0x80, "FRAMEBUFFER layout");
static_assert(sizeof(FRAMEBUFFER) == 0xf0, "FRAMEBUFFER is 0xf0 bytes");

// A lone target borrowed for a pass (0x38 bytes).
struct FRAMEBUFFERTEMP {
    int32_t m_width;                // 0x00
    int32_t m_height;               // 0x04
    FRAMEBUFFERTARGET m_target;     // 0x08
    int32_t m_size[2];              // 0x1c
    uint8_t m_pad24[4];
    void* m_tex;                    // 0x28
    uint8_t m_entry;                // 0x30
    uint8_t m_pad31[3];
    int32_t m_refs;                 // 0x34
};
static_assert(sizeof(FRAMEBUFFERTEMP) == 0x38, "FRAMEBUFFERTEMP is 0x38 bytes");

// Registers the cache's callbacks, CVar and console command (FUN_14129fcb0) and frees it all
// (FUN_14129f4c0).
void FrameBufferCacheInitialize();
void FrameBufferCacheShutdown();

// Runs the flush callbacks, then frees the cached textures; `all` also drops the entries and
// their heaps (FUN_1412a0050).
void FrameBufferCacheFlush(bool all);

// Callbacks run before a flush (FUN_14129f1d0, FUN_1412a0180).
void FrameBufferCacheAddFlushCallback(void (*callback)(void*), void* param);
void FrameBufferCacheRemoveFlushCallback(void (*callback)(void*), void* param);

// A cached texture for `target` at `size`, marking its entry in use (FUN_14129f7c0).
void* FrameBufferCacheAcquire(const int32_t* size, const FRAMEBUFFERTARGET* target, void* unused, uint8_t* entry);

// Frame buffer setup: one colour target of `format` and no depth at `size` (FUN_1412a04d0,
// the size packed as two 16-bit halves).
void FrameBufferSetup(FRAMEBUFFER* buffer, uint8_t format, uint32_t size);
// The same with the colour and depth formats of a render target kind (FUN_1412a0380).
void FrameBufferSetupKind(FRAMEBUFFER* buffer, const int32_t* size, uint32_t kind);

// Takes targets from the cache for whatever is not supplied, and builds the target set
// (FUN_14129fd70).
void FrameBufferBind(FRAMEBUFFER* buffer);

// Hands colour target `index` back and puts `tex` in its place (FUN_1412a05e0).
void FrameBufferSetColor(FRAMEBUFFER* buffer, uint32_t index, void* tex);

// Hands every cached target back (FUN_1412a0660), and only the depth target (FUN_1412a07c0).
void FrameBufferRelease(FRAMEBUFFER* buffer);
void FrameBufferReleaseDepth(FRAMEBUFFER* buffer);

// Swaps colour target `index` with a borrowed target (FUN_1412a0560).
void FrameBufferSwapColor(FRAMEBUFFER* buffer, uint32_t index, FRAMEBUFFERTEMP* temp);

// A borrowed target: built from a size and target (FUN_14129ef20) or a format (FUN_14129ef70),
// taken from the cache on first use (FUN_14129feb0) and handed back on last (FUN_1412a0770).
void FrameBufferTempInit(FRAMEBUFFERTEMP* temp, const char* name, const int32_t* size, const FRAMEBUFFERTARGET* target);
void FrameBufferTempInitFormat(FRAMEBUFFERTEMP* temp, const char* name, const int32_t* size, uint8_t format);
void FrameBufferTempAcquire(FRAMEBUFFERTEMP* temp);
void FrameBufferTempRelease(FRAMEBUFFERTEMP* temp);

// What the frame passes run on the colour target 0 of `buffer` while copying, flipping, or
// reading and resolving it through a borrowed target (FUN_1412a0cf0, FUN_1412a0e30,
// FUN_1412a0f80).
struct FRAMEBUFFERFLIP {
    GxRenderContext* m_context;     // 0x00
    FRAMEBUFFER* m_buffer;          // 0x08
    void* m_dest;                   // 0x10: a texture to flip into, else a borrowed one
    uint8_t m_flags;                // 0x18: bit 0 not a shader resource
};
typedef void (*FRAMEBUFFEREXECUTE)(void* param, GxRenderContext* context, void* source, void* dest);
void FrameBufferExecuteCopyDiscard(GxRenderContext* context, FRAMEBUFFER* buffer, FRAMEBUFFEREXECUTE execute, void* param);
void FrameBufferExecuteFlipDiscard(FRAMEBUFFERFLIP* flip, FRAMEBUFFEREXECUTE execute, void* param);
void FrameBufferExecuteReadOrResolveDiscard(GxRenderContext* context, FRAMEBUFFER* buffer, FRAMEBUFFEREXECUTE execute,
    void* param);

#endif
