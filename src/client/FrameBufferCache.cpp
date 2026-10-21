#include "client/FrameBufferCache.hpp"
#include "blz/Vector.hpp"
#include "client/RenderSettings.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/d3d12/Objects.hpp"
#include "storm/Memory.hpp"
#include <cstdlib>
#include <cstring>

namespace {

const char s_entryTag[] = ".?AUCacheEntry@FrameBufferCache@@";
const char s_texPtrTag[] = ".?AV?$unique_ptr@UGxTex@@UGxResourceDelete@@@blz@@";
const char s_callbackTag[] = ".?AU?$pair@P6AXPEAX@ZPEAX@blz@@";
const char s_callbackMapTag[] = ".PEAU?$pair@P6AXPEAX@ZPEAX@blz@@";

// A cached target's storage (0x28 bytes): with heaps on, one heap per entry with the
// textures placed in it; without, one texture.
struct CACHEENTRY {
    uint8_t m_inUse;                // 0x00
    uint8_t m_01[3];
    uint32_t m_heapKind;            // 0x04: 7 none
    void* m_heap;                   // 0x08: a GxTexHeap
    blz::vector<void*, s_texPtrTag> m_texs;  // 0x10
};
static_assert(sizeof(CACHEENTRY) == 0x28, "CACHEENTRY is 0x28 bytes");

struct HEAPSIZE {
    uint64_t m_size;
    uint64_t m_alignment;
};

struct FLUSHCALLBACK {
    void (*m_callback)(void*);
    void* m_param;
};

blz::vector<CACHEENTRY, s_entryTag> s_entries;  // DAT_1430a5920
HEAPSIZE s_heapSizes[7];                         // DAT_1430a58b0: by heap kind
blz::deque<FLUSHCALLBACK, 8, s_callbackMapTag, s_callbackTag> s_flushCallbacks;  // DAT_1430a5938
CVar* s_forceNoHeaps;                            // DAT_1430a5958
float s_scale;                                   // DAT_1430a5960: the render scale the heaps are for
GXSIZE s_screen;                                 // DAT_1430a5964: and the back buffer

bool HeapsEnabled() {
    return GxDeviceCaps()[0xaa] != 0 && s_forceNoHeaps->m_intValue == 0;
}

// Frees an entry's textures and heap.
// ref: FUN_14129ea80
void EntryDestroy(CACHEENTRY* entry) {
    for (uint64_t i = 0; i < entry->m_texs.m_size; i++) {
        GxTexDestroy(&entry->m_texs.m_data[i]);  // FUN_1412a0a60
    }
    entry->m_texs.m_size = 0;
    void* heap = entry->m_heap;
    entry->m_heap = nullptr;
    GxTexHeapDestroy(heap);
}

// Which heap a target goes in: by bytes per pixel 0 (4), 2 (8) or 4 (16), one more for a
// target bigger than the scaled screen, 6 when no heap fits.
// ref: FUN_14129f550
uint32_t HeapKind(const FRAMEBUFFERTARGET* target, GXSIZE size) {
    static const int32_t s_bytes[0x23] = { 0, 4, 4, 2, 2, 2, 2, 2, 4, 4, 2, 4, 1, 1, 2, 4, 4, 2, 2, 4, 8, 8, 4, 4, 4,
        0x10, 4, 2, 4, 4, 8, 4, 4, 4, 1 };
    static const uint32_t s_limits[7] = { 4, 4, 8, 8, 0x10, 0x10, 0x10 };
    uint8_t format = target->m_format;
    int32_t scale = (format >= 7 && format <= 0xb) ? 4 : 1;
    uint32_t kind = 0;
    while (static_cast<uint32_t>(scale * s_bytes[format]) > s_limits[kind]) {
        kind += 2;
        if (kind > 5) {
            return 6;
        }
    }
    uint16_t scaledWidth = static_cast<uint16_t>(static_cast<int64_t>(static_cast<float>(s_screen.m_width) * s_scale));
    uint16_t scaledHeight = static_cast<uint16_t>(static_cast<int64_t>(static_cast<float>(s_screen.m_height) * s_scale));
    if (scaledWidth < size.m_width && scaledHeight < size.m_height) {
        uint16_t width = s_screen.m_width;
        uint16_t height = s_screen.m_height;
        if (((kind - 1) & 1) == 0 || 1.0f < s_scale) {
            width = scaledWidth;
            height = scaledHeight;
        }
        if (width < size.m_width && height < size.m_height) {
            return 6;
        }
        if (target->m_samples != 0) {
            return 6;
        }
        return kind + 1;
    }
    return kind;
}

// A free entry with a texture that fits, or a free heap entry of the right kind to place a new
// one in (tex null).
// ref: FUN_14129fb00
bool FindEntry(const int32_t* size, const FRAMEBUFFERTARGET* target, void** tex, CACHEENTRY** found) {
    uint32_t kind = HeapKind(target, { static_cast<uint16_t>(size[0]), static_cast<uint16_t>(size[1]) });
    for (uint64_t e = 0; e < s_entries.m_size; e++) {
        CACHEENTRY* entry = &s_entries.m_data[e];
        if (entry->m_inUse != 0 || (entry->m_heap != nullptr && entry->m_heapKind != kind)) {
            continue;
        }
        for (uint64_t i = 0; i < entry->m_texs.m_size; i++) {
            auto texture = static_cast<const gxDx12Backend::Tex*>(entry->m_texs.m_data[i]);
            if (texture->m_width != static_cast<uint32_t>(size[0]) || texture->m_height != static_cast<uint32_t>(size[1])) {
                continue;
            }
            uint8_t have = texture->m_format;
            uint8_t want = target->m_format;
            if (want != have && !(want == 2 && have == 1) && !(want == 1 && have == 2) && !(want == 0x1a && have == 0x1d)) {
                continue;
            }
            uint32_t usage = texture->m_usage;
            if (((usage >> 0x15) & 7) == target->m_samples && ((usage >> 0x18) & 3) == target->m_08 &&
                target->m_renderTarget == ((usage >> 0x12) & 1) &&
                (target->m_unordered == ((usage >> 0x14) & 1) || (target->m_0f != 0 && entry->m_heap == nullptr)) &&
                (target->m_shaderResource == 0 || ((usage >> 0x13) & 1) != 0) && target->m_0f == ((usage >> 0x1b) & 1) &&
                target->m_10 == ((texture->m_usage2 >> 2) & 1)) {
                *tex = entry->m_texs.m_data[i];
                *found = entry;
                return true;
            }
        }
        if (entry->m_heap != nullptr) {
            *tex = nullptr;
            *found = entry;
            return false;
        }
    }
    *found = nullptr;
    return false;
}

// Sizes the heaps for the current render settings: each kind's largest target at the scaled
// (even kinds) or full (odd kinds) screen size.
// ref: FUN_14129f260
void ComputeHeapSizes(void* param) {
    (void)param;
    s_scale = g_renderSettings.m_renderScale;
    std::memset(s_heapSizes, 0, sizeof(s_heapSizes));
    s_screen = g_renderSettings.m_resolution;
    // The CVar is read last: this first runs from the device-created registration, before the
    // CVar exists, and the heap flag is clear then.
    if (GxDeviceCaps()[0xaa] == 0 || (g_renderSettings.m_flags & 0x200) == 0 || s_forceNoHeaps->m_intValue != 0) {
        return;
    }
    gxDx12Backend::TEXDESC desc = {};
    desc.m_dimension = 2;
    desc.m_width = 1;
    desc.m_height = 1;
    desc.m_depth = 1;
    desc.m_usage = 0xc2001;
    static const char s_formats[7] = { 2, 2, 0x14, 0x14, 0x19, 0x19, '#' };
    for (uint32_t kind = 0; kind < 7; kind++) {
        char format = s_formats[kind];
        if (format == '#') {
            continue;
        }
        desc.m_format = static_cast<uint8_t>(format);
        desc.m_11 = static_cast<uint8_t>(format);
        if ((kind & 1) == 0 || 1.0f < s_scale) {
            desc.m_width = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(s_screen.m_width) * s_scale)) & 0xffff;
            desc.m_height = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(s_screen.m_height) * s_scale)) & 0xffff;
            if ((kind & 1) == 0) {
                desc.m_usage = ((g_renderSettings.m_msaaSamples & 7u) << 0x15) | (desc.m_usage & 0xff1fffff);
            } else {
                desc.m_usage &= 0xff1fffff;
            }
        } else {
            desc.m_usage &= 0xff1fffff;
            desc.m_width = s_screen.m_width;
            desc.m_height = s_screen.m_height;
        }
        const uint8_t unordered[2] = { 1, 0 };
        for (uint8_t uav : unordered) {
            desc.m_usage = ((uav & 1u) << 0x14) | (desc.m_usage & 0xffefffff);
            GXTEXALLOCINFO info;
            GxTexAllocationInfo(&info, &desc);
            uint64_t size = (info.m_size - 1 + info.m_alignment) & ~(static_cast<uint64_t>(info.m_alignment) - 1);
            if (s_heapSizes[kind].m_size < size) {
                s_heapSizes[kind].m_size = size;
                s_heapSizes[kind].m_alignment = info.m_alignment;
            }
        }
    }
}

// ref: 0x14129f540
void FlushAll(void* param) {
    (void)param;
    FrameBufferCacheFlush(true);
}

// ref: FUN_14129e8d0
bool OnForceNoHeapsChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)userData;
    bool was = oldValue != nullptr && std::atoi(oldValue) != 0;
    bool is = std::atoi(newValue) != 0;
    if (was != is) {
        FrameBufferCacheFlush(true);
        ComputeHeapSizes(nullptr);
    }
    return true;
}

// A new size, sample count or heap mode drops everything and re-sizes the heaps; a new render
// scale frees the textures.
// ref: FUN_14129ff60
void OnSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)param;
    bool heaps = HeapsEnabled();
    bool changed = previous->m_resolution.m_width != current->m_resolution.m_width ||
        previous->m_resolution.m_height != current->m_resolution.m_height;
    if (previous->m_msaaSamples != current->m_msaaSamples) {
        changed = true;
    }
    if (heaps) {
        if (((previous->m_flags ^ current->m_flags) >> 9 & 1) != 0) {
            changed = true;
        }
        if ((current->m_flags & 0x200) != 0 && s_scale < current->m_renderScale) {
            changed = true;
        }
    }
    if (!changed) {
        if (current->m_renderScale != previous->m_renderScale) {
            FrameBufferCacheFlush(!((current->m_flags & 0x200) != 0 && heaps));
        }
        return;
    }
    FrameBufferCacheFlush(true);
    ComputeHeapSizes(nullptr);
}

// The cache's size: each texture's footprint, or each heap's.
// ref: FUN_14129e920
int32_t CommandGpuFootprint(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    if (GxApiIsD3d11()) {
        ConsoleWriteText("Not supported on DX11");
        return 0;
    }
    uint64_t bytes = 0;
    uint32_t count = 0;
    for (uint64_t e = 0; e < s_entries.m_size; e++) {
        CACHEENTRY* entry = &s_entries.m_data[e];
        count++;
        if (entry->m_heap == nullptr) {
            for (uint64_t i = 0; i < entry->m_texs.m_size; i++) {
                uint8_t desc[0x30];
                GxTexGetDesc(entry->m_texs.m_data[i], desc);
                GXTEXALLOCINFO info;
                bytes += GxTexAllocationInfo(&info, desc)->m_size;
            }
        } else {
            bytes += s_heapSizes[entry->m_heapKind].m_size;
        }
    }
    ConsoleWriteFormat(0, "Count: %u, Size: %.2fMB", count, static_cast<double>(bytes) * (1.0 / 1048576.0));
    return 1;
}

GxRenderContext* Context() {
    return g_gxRenderContext;
}

}

// ref: FUN_14129fcb0
void FrameBufferCacheInitialize() {
    GxAddDeviceCreatedCallback(ComputeHeapSizes, nullptr);
    GxAddDeviceDestroyedCallback(FlushAll, nullptr);
    GxAddLowMemoryCallback(FlushAll, nullptr);
    RenderSettingsAddHandler(OnSettingsChanged, nullptr);
    s_forceNoHeaps = CVarRegister("FrameBufferCacheForceNoHeaps", "Disable use of texture heaps and force the fallback path",
        0x201, "0", OnForceNoHeapsChanged, 1, false, nullptr, false);
    ConsoleCommandRegister("FrameBufferCacheGpuFootprint", CommandGpuFootprint, 1, nullptr, 0);
}

// ref: FUN_14129f4c0
void FrameBufferCacheShutdown() {
    GxRemoveLowMemoryCallback(FlushAll, nullptr);
    GxRemoveDeviceCreatedCallback(ComputeHeapSizes, nullptr);
    GxRemoveDeviceDestroyedCallback(FlushAll, nullptr);
    RenderSettingsRemoveHandler(OnSettingsChanged, nullptr);
    FrameBufferCacheFlush(true);
    for (uint64_t e = 0; e < s_entries.m_size; e++) {
        EntryDestroy(&s_entries.m_data[e]);
    }
    s_entries.m_size = 0;
}

// ref: FUN_1412a0050
void FrameBufferCacheFlush(bool all) {
    for (uint64_t i = 0; i < s_flushCallbacks.size(); i++) {
        FLUSHCALLBACK& callback = s_flushCallbacks.at(i);
        callback.m_callback(callback.m_param);
    }
    if (!all) {
        for (uint64_t e = 0; e < s_entries.m_size; e++) {
            CACHEENTRY* entry = &s_entries.m_data[e];
            for (uint64_t i = 0; i < entry->m_texs.m_size; i++) {
                GxTexDestroy(&entry->m_texs.m_data[i]);
            }
            entry->m_texs.m_size = 0;
        }
        return;
    }
    for (uint64_t e = 0; e < s_entries.m_size; e++) {
        EntryDestroy(&s_entries.m_data[e]);
    }
    s_entries.m_size = 0;
}

// ref: FUN_14129f1d0
void FrameBufferCacheAddFlushCallback(void (*callback)(void*), void* param) {
    FLUSHCALLBACK* slot = s_flushCallbacks.push_back_slot();
    slot->m_callback = callback;
    slot->m_param = param;
}

// ref: FUN_1412a0180
void FrameBufferCacheRemoveFlushCallback(void (*callback)(void*), void* param) {
    for (uint64_t i = 0; i < s_flushCallbacks.size(); i++) {
        FLUSHCALLBACK& entry = s_flushCallbacks.at(i);
        if (entry.m_callback == callback && entry.m_param == param) {
            s_flushCallbacks.erase(i);
            return;
        }
    }
}

// ref: FUN_14129f7c0
void* FrameBufferCacheAcquire(const int32_t* size, const FRAMEBUFFERTARGET* target, void* unused, uint8_t* index) {
    (void)unused;
    if (target->m_format == 0) {
        return nullptr;
    }
    CACHEENTRY* entry = nullptr;
    void* tex = nullptr;
    if (!FindEntry(size, target, &tex, &entry)) {
        gxDx12Backend::TEXDESC desc = {};
        desc.m_dimension = 2;
        desc.m_width = static_cast<uint32_t>(size[0]);
        desc.m_height = static_cast<uint32_t>(size[1]);
        desc.m_depth = 1;
        desc.m_format = target->m_format;
        desc.m_11 = target->m_format;
        uint32_t usage = target->m_renderTarget != 0 ? 0x42001u : 0x2001u;
        usage |= ((((((target->m_0f & 1u) << 3) | (target->m_08 & 3u)) << 3 | (target->m_samples & 7u)) << 2) |
                     (target->m_unordered != 0 ? 2u : 0u) | (target->m_shaderResource & 1u))
            << 0x13;
        desc.m_usage = usage;
        desc.m_usage2 = (target->m_10 & 1u) << 2;
        if (entry == nullptr) {
            CACHEENTRY* added = &s_entries.emplace_back();
            added->m_inUse = 0;
            added->m_heapKind = 7;
            added->m_heap = nullptr;
            entry = added;
            entry->m_heapKind = HeapKind(target, { static_cast<uint16_t>(size[0]), static_cast<uint16_t>(size[1]) });
            HEAPSIZE* heapSize = &s_heapSizes[entry->m_heapKind];
            if (heapSize->m_size != 0) {
                TEXHEAPDESC heapDesc = {};
                heapDesc.m_size = heapSize->m_size;
                heapDesc.m_alignment = static_cast<uint32_t>(heapSize->m_alignment);
                heapDesc.m_targets = 1;
                void* heap = nullptr;
                GxTexHeapCreate(&heapDesc, &heap);
                void* old = entry->m_heap;
                entry->m_heap = heap;
                GxTexHeapDestroy(old);
            }
        }
        desc.m_heap = entry->m_heap;
        desc.m_28 = 0;
        entry->m_texs.push_back(nullptr);  // FUN_14129eb00
        void** slot = &entry->m_texs.m_data[entry->m_texs.m_size - 1];
        void* created = nullptr;
        GxTexCreate(&desc, &created);
        void* old = *slot;
        *slot = created;
        if (old != nullptr) {
            GxTexDestroy(&old);
        }
        tex = *slot;
    }
    *index = static_cast<uint8_t>(entry - s_entries.m_data);
    entry->m_inUse = 1;
    return tex;
}

// ref: FUN_1412a04d0
void FrameBufferSetup(FRAMEBUFFER* buffer, uint8_t format, uint32_t size) {
    buffer->m_width = static_cast<int32_t>(size & 0xffff);
    buffer->m_height = static_cast<int32_t>(size >> 0x10);
    buffer->m_size[0] = buffer->m_width;
    buffer->m_size[1] = buffer->m_height;
    std::memset(&buffer->m_color[0], 0, sizeof(FRAMEBUFFERTARGET));
    buffer->m_color[0].m_format = format;
    buffer->m_color[0].m_renderTarget = 1;
    buffer->m_color[0].m_shaderResource = 1;
    std::memset(&buffer->m_depth, 0, sizeof(FRAMEBUFFERTARGET));
    buffer->m_depth.m_renderTarget = 1;
    GxRenderTargetsClear(&buffer->m_targets);
}

// ref: FUN_1412a0380
void FrameBufferSetupKind(FRAMEBUFFER* buffer, const int32_t* size, uint32_t kind) {
    RENDERTARGETFORMATS formats = RenderTargetFormats(kind);
    buffer->m_width = size[0];
    buffer->m_height = size[1];
    buffer->m_size[0] = size[0];
    buffer->m_size[1] = size[1];
    std::memset(&buffer->m_color[0], 0, sizeof(FRAMEBUFFERTARGET));
    buffer->m_color[0].m_format = formats.m_color;
    buffer->m_color[0].m_renderTarget = 1;
    buffer->m_color[0].m_shaderResource = 1;
    std::memset(&buffer->m_depth, 0, sizeof(FRAMEBUFFERTARGET));
    buffer->m_depth.m_format = formats.m_depth;
    buffer->m_depth.m_renderTarget = 1;
    GxRenderTargetsClear(&buffer->m_targets);
}

// ref: FUN_14129fd70
void FrameBufferBind(FRAMEBUFFER* buffer) {
    int32_t limit = *reinterpret_cast<int32_t*>(GxDeviceCaps() + 0x98);
    int32_t width = buffer->m_width < limit ? buffer->m_width : limit;
    int32_t height = buffer->m_height < limit ? buffer->m_height : limit;
    buffer->m_size[0] = width > 0 ? width : 1;
    buffer->m_size[1] = height > 0 ? height : 1;
    for (uint32_t i = 0; i < 4; i++) {
        if (buffer->m_targets.m_texs[i + 1] == nullptr && buffer->m_color[i].m_format != 0) {
            void* tex = FrameBufferCacheAcquire(buffer->m_size, &buffer->m_color[i], nullptr, &buffer->m_colorEntries[i]);
            GxRenderTargetsAttach(&buffer->m_targets, static_cast<uint8_t>(i + 1), static_cast<gxDx12Backend::Tex*>(tex));
        } else {
            buffer->m_colorEntries[i] = 0xff;
        }
    }
    if (buffer->m_targets.m_texs[0] == nullptr && buffer->m_depth.m_format != 0) {
        void* tex = FrameBufferCacheAcquire(buffer->m_size, &buffer->m_depth, nullptr, &buffer->m_depthEntry);
        GxRenderTargetsAttach(&buffer->m_targets, 0, static_cast<gxDx12Backend::Tex*>(tex));
    } else {
        buffer->m_depthEntry = 0xff;
    }
    GxRenderTargetsCount(&buffer->m_targets);
    buffer->m_bound = 1;
}

// ref: FUN_1412a05e0
void FrameBufferSetColor(FRAMEBUFFER* buffer, uint32_t index, void* tex) {
    Context()->Slot020(2, buffer->m_targets.m_texs[index + 1]);
    uint8_t entry = buffer->m_colorEntries[index];
    if (entry != 0xff) {
        s_entries.m_data[entry].m_inUse = 0;
    }
    GxRenderTargetsSet(&buffer->m_targets, index + 1, static_cast<gxDx12Backend::Tex*>(tex));
    buffer->m_colorEntries[index] = 0xff;
}

// ref: FUN_1412a0660
void FrameBufferRelease(FRAMEBUFFER* buffer) {
    if (buffer->m_bound == 0) {
        return;
    }
    buffer->m_bound = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t entry = buffer->m_colorEntries[i];
        if (entry != 0xff) {
            Context()->Slot020(2, buffer->m_targets.m_texs[i + 1]);
            s_entries.m_data[entry].m_inUse = 0;
        }
        buffer->m_colorEntries[i] = 0xff;
    }
    if (buffer->m_depthEntry != 0xff) {
        s_entries.m_data[buffer->m_depthEntry].m_inUse = 0;
    }
    buffer->m_depthEntry = 0xff;
}

// ref: FUN_1412a07c0
void FrameBufferReleaseDepth(FRAMEBUFFER* buffer) {
    if (buffer->m_bound == 0) {
        return;
    }
    if (buffer->m_depthEntry != 0xff) {
        s_entries.m_data[buffer->m_depthEntry].m_inUse = 0;
    }
    GxRenderTargetsAttach(&buffer->m_targets, 0, nullptr);
    buffer->m_depthEntry = 0xff;
    GxRenderTargetsCount(&buffer->m_targets);
}

// ref: FUN_1412a0560
void FrameBufferSwapColor(FRAMEBUFFER* buffer, uint32_t index, FRAMEBUFFERTEMP* temp) {
    FRAMEBUFFERTARGET target = buffer->m_color[index];
    buffer->m_color[index] = temp->m_target;
    temp->m_target = target;
    uint8_t entry = buffer->m_colorEntries[index];
    buffer->m_colorEntries[index] = temp->m_entry;
    temp->m_entry = entry;
    void* tex = temp->m_tex;
    temp->m_tex = buffer->m_targets.m_texs[index + 1];
    GxRenderTargetsSet(&buffer->m_targets, index + 1, static_cast<gxDx12Backend::Tex*>(tex));
}

// ref: FUN_14129ef20
void FrameBufferTempInit(FRAMEBUFFERTEMP* temp, const char* name, const int32_t* size, const FRAMEBUFFERTARGET* target) {
    (void)name;
    std::memset(temp, 0, sizeof(*temp));
    temp->m_entry = 0xff;
    temp->m_width = size[0];
    temp->m_height = size[1];
    temp->m_target = *target;
}

// ref: FUN_14129ef70
void FrameBufferTempInitFormat(FRAMEBUFFERTEMP* temp, const char* name, const int32_t* size, uint8_t format) {
    (void)name;
    std::memset(temp, 0, sizeof(*temp));
    temp->m_entry = 0xff;
    temp->m_width = size[0];
    temp->m_height = size[1];
    temp->m_target.m_format = format;
    temp->m_target.m_renderTarget = 1;
    temp->m_target.m_shaderResource = 1;
}

// ref: FUN_14129feb0
void FrameBufferTempAcquire(FRAMEBUFFERTEMP* temp) {
    if (temp->m_refs == 0) {
        int32_t limit = *reinterpret_cast<int32_t*>(GxDeviceCaps() + 0x98);
        int32_t width = limit <= temp->m_width ? limit : temp->m_width;
        int32_t height = limit <= temp->m_height ? limit : temp->m_height;
        temp->m_size[0] = width > 1 ? width : 1;
        temp->m_size[1] = height > 1 ? height : 1;
        if (temp->m_tex == nullptr && temp->m_target.m_format != 0) {
            temp->m_tex = FrameBufferCacheAcquire(temp->m_size, &temp->m_target, nullptr, &temp->m_entry);
        }
    }
    temp->m_refs++;
}

// ref: FUN_1412a5c10
void FrameBufferInit(FRAMEBUFFER* buffer) {
    std::memset(buffer, 0, sizeof(*buffer));
    for (FRAMEBUFFERTARGET& target : buffer->m_color) {
        target.m_renderTarget = 1;
    }
    buffer->m_depth.m_renderTarget = 1;
    for (uint8_t& entry : buffer->m_colorEntries) {
        entry = 0xff;
    }
    buffer->m_depthEntry = 0xff;
}

void FrameBufferTempClear(FRAMEBUFFERTEMP* temp) {
    temp->m_width = 0;
    temp->m_height = 0;
    std::memset(&temp->m_target, 0, sizeof(temp->m_target));
    temp->m_target.m_renderTarget = 1;
    temp->m_size[0] = 0;
    temp->m_size[1] = 0;
    temp->m_tex = nullptr;
    temp->m_entry = 0xff;
    temp->m_refs = 0;
}

// ref: FUN_1412a0770
void FrameBufferTempRelease(FRAMEBUFFERTEMP* temp) {
    temp->m_refs--;
    if (temp->m_refs == 0 && temp->m_tex != nullptr) {
        Context()->Slot020(2, temp->m_tex);
        if (temp->m_entry != 0xff) {
            s_entries.m_data[temp->m_entry].m_inUse = 0;
        }
    }
}

namespace {

// Colour target 0 of `buffer` as a single-sampled target to copy or resolve into.
FRAMEBUFFERTARGET SingleSampled(const FRAMEBUFFER* buffer, bool multisampled) {
    FRAMEBUFFERTARGET target = buffer->m_color[0];
    target.m_samples = 0;
    if (multisampled) {
        target.m_shaderResource = 1;
    }
    return target;
}

bool Multisampled(const void* tex) {
    return (static_cast<const gxDx12Backend::Tex*>(tex)->m_usage & 0xe00000) != 0;
}

}

// ref: FUN_1412a0cf0
void FrameBufferExecuteCopyDiscard(GxRenderContext* context, FRAMEBUFFER* buffer, FRAMEBUFFEREXECUTE execute, void* param) {
    auto source = static_cast<gxDx12Backend::Tex*>(buffer->m_targets.m_texs[1]);
    bool multisampled = Multisampled(source);
    FRAMEBUFFERTARGET target = SingleSampled(buffer, multisampled);
    FRAMEBUFFERTEMP temp;
    FrameBufferTempInit(&temp, "ExecuteCopyDiscard", buffer->m_size, &target);
    FrameBufferTempAcquire(&temp);
    if (!multisampled) {
        int32_t origin[2] = { 0, 0 };
        int32_t destOrigin[2] = { 0, 0 };
        uint32_t size[2] = { static_cast<uint32_t>(buffer->m_size[0]), static_cast<uint32_t>(buffer->m_size[1]) };
        context->CopyTex(source, static_cast<gxDx12Backend::Tex*>(temp.m_tex), origin, destOrigin, size, 0, 0);
    } else {
        context->ResolveTex(source, static_cast<gxDx12Backend::Tex*>(temp.m_tex));
    }
    execute(param, context, temp.m_tex, source);
    FrameBufferTempRelease(&temp);
}

// ref: FUN_1412a0e30
void FrameBufferExecuteFlipDiscard(FRAMEBUFFERFLIP* flip, FRAMEBUFFEREXECUTE execute, void* param) {
    FRAMEBUFFER* buffer = flip->m_buffer;
    GxRenderContext* context = flip->m_context;
    void* source = buffer->m_targets.m_texs[1];
    if (flip->m_dest != nullptr) {
        execute(param, context, source, flip->m_dest);
        FrameBufferSetColor(buffer, 0, flip->m_dest);
        return;
    }
    bool multisampled = Multisampled(source);
    FRAMEBUFFERTARGET target = SingleSampled(buffer, multisampled);
    if ((flip->m_flags & 1) != 0) {
        target.m_unordered = 0;
    }
    FRAMEBUFFERTEMP temp;
    FrameBufferTempInit(&temp, "ExecuteFlipDiscard", buffer->m_size, &target);
    FrameBufferTempAcquire(&temp);
    if (!multisampled) {
        execute(param, context, source, temp.m_tex);
        FrameBufferSwapColor(buffer, 0, &temp);
    } else {
        context->ResolveTex(static_cast<gxDx12Backend::Tex*>(source), static_cast<gxDx12Backend::Tex*>(temp.m_tex));
        execute(param, context, temp.m_tex, source);
    }
    FrameBufferTempRelease(&temp);
}

// ref: FUN_1412a0f80
void FrameBufferExecuteReadOrResolveDiscard(GxRenderContext* context, FRAMEBUFFER* buffer, FRAMEBUFFEREXECUTE execute,
    void* param) {
    void* source = buffer->m_targets.m_texs[1];
    if (buffer->m_color[0].m_samples == 0) {
        execute(param, context, source, nullptr);
        return;
    }
    FRAMEBUFFERTARGET target = SingleSampled(buffer, true);
    FRAMEBUFFERTEMP temp;
    FrameBufferTempInit(&temp, "ExecuteReadOrResolveDiscard", buffer->m_size, &target);
    FrameBufferTempAcquire(&temp);
    context->ResolveTex(static_cast<gxDx12Backend::Tex*>(source), static_cast<gxDx12Backend::Tex*>(temp.m_tex));
    execute(param, context, temp.m_tex, nullptr);
    FrameBufferTempRelease(&temp);
}

// ref: FUN_1412a0340
void FrameBufferSetupTargets(FRAMEBUFFER* buffer, const int32_t* size, const FRAMEBUFFERTARGET* color,
    const FRAMEBUFFERTARGET* depth) {
    buffer->m_width = size[0];
    buffer->m_height = size[1];
    buffer->m_size[0] = size[0];
    buffer->m_size[1] = size[1];
    buffer->m_color[0] = *color;
    buffer->m_depth = *depth;
    GxRenderTargetsClear(&buffer->m_targets);
}

// Two colour targets of the default kind's formats; the depth is a target of its own when the
// scene is multisampled, else the depth texture of `shared` itself.
// ref: FUN_1412b2130
void FrameBufferSetupSharedDepth(FRAMEBUFFER* buffer, const int32_t* size, GXRENDERTARGETS* shared) {
    RENDERTARGETFORMATS formats = RenderTargetFormats(0);
    buffer->m_width = size[0];
    buffer->m_height = size[1];
    FRAMEBUFFERTARGET depth = {};
    depth.m_renderTarget = 1;
    if (g_renderSettings.m_msaaSamples != 0) {
        depth.m_format = formats.m_depth;
        depth.m_shaderResource = 1;
        depth.m_10 = 1;
        buffer->m_depth = depth;
    } else {
        gxDx12Backend::Tex* tex = shared->m_texs[0];
        uint32_t usage = tex->m_usage;
        depth.m_format = tex->m_format;
        depth.m_samples = (usage >> 0x15) & 7;
        depth.m_08 = (usage >> 0x18) & 3;
        depth.m_unordered = (usage >> 0x14) & 1;
        depth.m_shaderResource = (usage >> 0x13) & 1;
        buffer->m_depth = depth;
        GxRenderTargetsAttach(&buffer->m_targets, 0, tex);
    }
    FRAMEBUFFERTARGET color = {};
    color.m_format = formats.m_color;
    color.m_renderTarget = 1;
    color.m_shaderResource = 1;
    buffer->m_color[0] = color;
    color.m_format = formats.m_01[0];
    buffer->m_color[1] = color;
}

// Takes over `source`'s targets and cache entries; `source` is left holding none.
// ref: FUN_14129f070
FRAMEBUFFER* FrameBufferMove(FRAMEBUFFER* dest, FRAMEBUFFER* source) {
    *dest = *source;
    for (uint8_t& entry : source->m_colorEntries) {
        entry = 0xff;
    }
    source->m_depthEntry = 0xff;
    source->m_bound = 0;
    return dest;
}

// A single-sampled copy of `source`'s first colour target in `dest` (released from the
// cache first), then `source` handed back.
// ref: FUN_1412a1070
void FrameBufferResolve(GxRenderContext* context, FRAMEBUFFER* source, FRAMEBUFFER* dest) {
    int32_t size[2] = {source->m_size[0], source->m_size[1]};
    FRAMEBUFFERTARGET color = source->m_color[0];
    color.m_samples = 0;
    color.m_shaderResource = 1;
    FRAMEBUFFERTARGET depth = {};
    depth.m_renderTarget = 1;
    FrameBufferSetupTargets(dest, size, &color, &depth);
    FrameBufferBind(dest);
    context->ResolveTex(source->m_targets.m_texs[1], dest->m_targets.m_texs[1]);
    FrameBufferRelease(source);
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14129eb00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412a0a60=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14129f260 = (void*)&ComputeHeapSizes;
extern "C" void* const gen_p_1430a5920 = (void*)&s_entries;
extern "C" void* const gen_p_1430a5938 = (void*)&s_flushCallbacks;
extern "C" void* const gen_p_1430a5958 = (void*)&s_forceNoHeaps;
extern "C" void* const gen_p_1430a5960 = (void*)&s_scale;
extern "C" void* const gen_p_1430a5964 = (void*)&s_screen;
// ---- end of generated globals ----
