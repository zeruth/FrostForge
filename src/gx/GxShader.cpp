#include "gx/GxShader.hpp"
#include "cas/CAS.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "storm/Memory.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include "zlib.h"
#include <windows.h>
#include <cstring>

extern const char s_shaderBufferTag[] = ".E";

namespace {

#include "gx/GxShaderTables.inc"

const char s_shaderFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\GxShader.cpp";
const char s_smemNewFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Storm\\H\\SMemNew.h";
const char s_scompFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Storm\\Source\\SComp.cpp";

// The tag of each family's blob in a shader container (DAT_142913650): "DX04", "DX05",
// "DX06", "MT11".
const uint32_t s_familyTags[4] = { 0x34305844, 0x35305844, 0x36305844, 0x3131544d };

// What GetComputePermute returns for a permute the build deleted (DAT_142913660).
const GXSHADERPERMUTE s_deletedPermute = {};

int32_t s_family = 4;               // DAT_142d438c0: 4 until a device sets one
uint8_t s_cacheless;                // DAT_143490a18
uint8_t s_loading;                  // DAT_143490a19: preloading, errors are not logged

// The shaders loaded for the family, by FileDataID (DAT_142d438c8): blz's open-addressing map
// of int keys (0 empty, -1 removed) to the loaded blob.
struct SHADERENTRY {
    int32_t m_id;
    int32_t m_pad;
    uint8_t* m_blob;
};
struct SHADERMAP {
    uint64_t m_capacity;
    SHADERENTRY* m_entries;
    uint64_t m_size;
    uint64_t m_removed;
    float m_maxLoad;
};
SHADERMAP s_shaders = { 0, nullptr, 0, 0, 0.5f };
const char s_shaderMapTag[] = ".?AU?$pair@$$CBHUGxShader@@@blz@@";

// The ones loaded on demand once the preload is done (DAT_142d438f0): blz's chained map.
struct SHADERNODE {
    SHADERNODE* m_next;
    int32_t m_id;
    int32_t m_pad;
    uint8_t* m_blob;
};
struct LATEMAP {
    uint64_t m_bucketCount;
    SHADERNODE** m_buckets;
    uint64_t m_size;
    float m_maxLoad;
};
LATEMAP s_lateShaders = { 0, nullptr, 0, 1.0f };
SRWLOCK s_lateLock = SRWLOCK_INIT;  // DAT_143490a10
const char s_lateNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBHUGxShader@@@blz@@@blz@@";
const char s_lateBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBHUGxShader@@@blz@@@blz@@";

// Rehashes the shader map for `count` entries (FUN_142295000).
void ShaderMapRehash(SHADERMAP* map, uint64_t count) {
    float needed = static_cast<float>(map->m_size - map->m_removed) / map->m_maxLoad;
    uint64_t wanted = static_cast<uint64_t>(needed);
    if (static_cast<float>(map->m_size - map->m_removed) > static_cast<float>(wanted) * map->m_maxLoad) {
        wanted++;
    }
    if (wanted < count) {
        wanted = count;
    }
    uint64_t capacity = 8;
    while (capacity < wanted) {
        capacity *= 2;
    }
    if (capacity == map->m_capacity && map->m_removed == 0) {
        return;
    }
    auto entries = static_cast<SHADERENTRY*>(SMemAlloc(capacity * sizeof(SHADERENTRY), 0xb, s_shaderMapTag,
        SStrHashFnv1a(s_shaderMapTag), -6, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_id = 0;
    }
    for (uint64_t i = 0; i < map->m_capacity; i++) {
        SHADERENTRY* old = &map->m_entries[i];
        if (old->m_id == 0 || (map->m_removed != 0 && old->m_id == -1)) {
            continue;
        }
        uint64_t index = static_cast<uint64_t>(static_cast<int64_t>(old->m_id)) & (capacity - 1);
        for (uint64_t step = 0; entries[index].m_id != 0;) {
            step++;
            index = (index + step) & (capacity - 1);
        }
        entries[index] = *old;
    }
    if (map->m_entries != nullptr) {
        SMemFree(map->m_entries, s_shaderMapTag, -6, 0);
    }
    map->m_size -= map->m_removed;
    map->m_removed = 0;
    map->m_entries = entries;
    map->m_capacity = capacity;
}

// The entry for `id`, added empty when missing.
uint8_t** ShaderMapInsert(SHADERMAP* map, int32_t id) {
    if (map->m_capacity == 0 || map->m_maxLoad < static_cast<float>(map->m_size + 1) / static_cast<float>(map->m_capacity)) {
        ShaderMapRehash(map, 0);
    }
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = static_cast<uint64_t>(static_cast<int64_t>(id)) & mask;
    SHADERENTRY* removed = nullptr;
    for (uint64_t step = 0;; step++) {
        SHADERENTRY* entry = &map->m_entries[index];
        if (entry->m_id == 0) {
            if (removed != nullptr) {
                entry = removed;
            }
            entry->m_id = id;
            entry->m_blob = nullptr;
            map->m_size++;
            return &entry->m_blob;
        }
        if (entry->m_id == -1) {
            if (removed == nullptr) {
                removed = entry;
            }
        } else if (entry->m_id == id) {
            return &entry->m_blob;
        }
        index = (index + step + 1) & mask;
    }
}

uint8_t** ShaderMapFind(SHADERMAP* map, int32_t id) {
    if (map->m_capacity == 0) {
        return nullptr;
    }
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = static_cast<uint64_t>(static_cast<int64_t>(id)) & mask;
    for (uint64_t step = 0; map->m_entries[index].m_id != 0; step++) {
        SHADERENTRY* entry = &map->m_entries[index];
        if ((map->m_removed == 0 || entry->m_id != -1) && entry->m_id == id) {
            return &entry->m_blob;
        }
        index = (index + step + 1) & mask;
    }
    return nullptr;
}

// The chained map's node for `id`, added when missing (FUN_1422939f0, FUN_142294e10).
uint8_t** LateMapInsert(int32_t id, bool* inserted) {
    LATEMAP* map = &s_lateShaders;
    if (map->m_buckets != nullptr) {
        for (SHADERNODE* node = map->m_buckets[static_cast<uint64_t>(static_cast<int64_t>(id)) & (map->m_bucketCount - 1)];
             node != nullptr; node = node->m_next) {
            if (node->m_id == id) {
                *inserted = false;
                return &node->m_blob;
            }
        }
    }
    if (map->m_buckets == nullptr || map->m_maxLoad < static_cast<float>(map->m_size + 1) / static_cast<float>(map->m_bucketCount)) {
        uint64_t wanted = static_cast<uint64_t>(static_cast<float>(map->m_size + 1) / map->m_maxLoad) + 1;
        uint64_t count = 8;
        while (count < wanted) {
            count *= 2;
        }
        auto buckets = static_cast<SHADERNODE**>(SMemAlloc(count * 8, 0xb, s_lateBucketTag, SStrHashFnv1a(s_lateBucketTag), -6, 0));
        std::memset(buckets, 0, count * 8);
        for (uint64_t i = 0; map->m_buckets != nullptr && i < map->m_bucketCount; i++) {
            SHADERNODE* node = map->m_buckets[i];
            while (node != nullptr) {
                SHADERNODE* next = node->m_next;
                uint64_t slot = static_cast<uint64_t>(static_cast<int64_t>(node->m_id)) & (count - 1);
                node->m_next = buckets[slot];
                buckets[slot] = node;
                node = next;
            }
        }
        if (map->m_buckets != nullptr) {
            SMemFree(map->m_buckets, s_lateBucketTag, -6, 0);
        }
        map->m_buckets = buckets;
        map->m_bucketCount = count;
    }
    auto node = static_cast<SHADERNODE*>(SMemAlloc(sizeof(SHADERNODE), 0xb, s_lateNodeTag, SStrHashFnv1a(s_lateNodeTag), -6, 0));
    node->m_id = id;
    node->m_blob = nullptr;
    uint64_t slot = static_cast<uint64_t>(static_cast<int64_t>(id)) & (map->m_bucketCount - 1);
    node->m_next = map->m_buckets[slot];
    map->m_buckets[slot] = node;
    map->m_size++;
    *inserted = true;
    return &node->m_blob;
}

void ShaderLog(const char* format, int32_t id, const char* message = nullptr) {
    char name[0x100];
    GxShaderName(id, name, sizeof(name));
    if (message == nullptr) {
        GxLog(format, name);
    } else {
        GxLog(format, name, message);
    }
}

// Takes the family's blob out of a "TAFG" container and checks its "HSXG" header.
bool ShaderExtractFamily(uint8_t** blob, uint32_t size, int32_t family) {
    if (size < 0xc) {
        return false;
    }
    auto header = reinterpret_cast<int32_t*>(*blob);
    if (header[0] == 0x47464154) {
        if (header[1] != 0x1000b) {
            return false;
        }
        uint32_t count = static_cast<uint32_t>(header[2]);
        if (count > 0x80 || size < static_cast<uint64_t>(count) * 0xc || count == 0) {
            return false;
        }
        int32_t begin = 0;
        int32_t end = 0;
        auto entry = reinterpret_cast<uint32_t*>(header + 3);
        for (uint32_t i = 0; i < count; i++, entry += 3) {
            if (entry[0] == s_familyTags[family]) {
                begin = static_cast<int32_t>(entry[1]);
                end = static_cast<int32_t>(entry[2]);
            }
        }
        if (begin < 1 || end < 1 || end <= begin || static_cast<int32_t>(size) <= begin || static_cast<int32_t>(size) < end) {
            return false;
        }
        auto copy = static_cast<uint8_t*>(SMemAlloc(static_cast<uint64_t>(end - begin), 0xd, s_shaderFile,
            SStrHashFnv1a(s_shaderFile), 0x213, 0));
        std::memcpy(copy, *blob + begin, static_cast<size_t>(end - begin));
        uint8_t* old = *blob;
        *blob = copy;
        if (old != nullptr) {
            SMemFree(old, s_smemNewFile, 0xa8, 0);
        }
    }
    header = reinterpret_cast<int32_t*>(*blob);
    return size > 0x1b && header[0] == 0x47585348 && header[1] == 0x1000c && static_cast<uint32_t>(header[2]) <= size &&
        static_cast<uint32_t>(header[4]) <= size && static_cast<uint32_t>(header[6]) <= size;
}

// Loads a shader's blob for the family into `slot` once.
// ref: FUN_142294b60
bool ShaderLoad(uint8_t** slot, int32_t id, int32_t family) {
    if (*slot != nullptr) {
        return true;
    }
    if (id == 0) {
        return false;
    }
    if (!cas::CASFileExists(id)) {
        return false;
    }
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!cas::CASFileLoadById(id, &data, &size, 0, 0xd)) {
        if (s_loading == 0) {
            char message[0x100];
            cas::CASFormatLastError(message, sizeof(message));
            ShaderLog("Error Loading Shader: %s. Error Message: %s", id, message);
        }
        return false;
    }
    uint8_t* old = *slot;
    *slot = data;
    if (old != nullptr) {
        SMemFree(old, s_smemNewFile, 0xa8, 0);
    }
    if (ShaderExtractFamily(slot, size, family)) {
        return true;
    }
    if (s_loading == 0) {
        ShaderLog("Error Loading Shader: %s", id);
    }
    return false;
}

// Storm's inflate arena: zlib's allocations come from a buffer on the stack, falling back to
// Storm when it runs out (FUN_14235a910, LAB_14235a9a0).
struct INFLATEARENA {
    uint8_t* m_cursor;
    uint32_t m_size;
    uint8_t* m_base;
};

voidpf ArenaAlloc(voidpf opaque, uInt items, uInt size) {
    auto arena = static_cast<INFLATEARENA*>(opaque);
    uint32_t bytes = items * size;
    if ((bytes & 7) != 0) {
        bytes += 8 - (bytes & 7);
    }
    if (static_cast<int64_t>(bytes) < static_cast<int64_t>(arena->m_size + arena->m_base - arena->m_cursor)) {
        void* result = arena->m_cursor;
        arena->m_cursor += bytes;
        return result;
    }
    return SMemAlloc(bytes, 7, s_scompFile, SStrHashFnv1a(s_scompFile), 0x1f, 0);
}

void ArenaFree(voidpf opaque, voidpf address) {
    auto arena = static_cast<INFLATEARENA*>(opaque);
    auto bytes = static_cast<uint8_t*>(address);
    if (bytes < arena->m_base || arena->m_base + arena->m_size <= bytes) {
        SMemFree(address, s_scompFile, 0x27, 0);
    }
}

// Inflates one shader block (FUN_14235aad0).
int32_t InflateBlock(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize) {
    uint8_t s_arena[300000];
    INFLATEARENA arena = { s_arena, 300000, s_arena };
    z_stream stream = {};
    stream.next_in = static_cast<Bytef*>(const_cast<void*>(source));
    stream.avail_in = sourceSize;
    stream.next_out = static_cast<Bytef*>(dest);
    stream.avail_out = *destSize;
    stream.zalloc = ArenaAlloc;
    stream.zfree = ArenaFree;
    stream.opaque = &arena;
    int result = inflateInit_(&stream, "1.2.11", sizeof(z_stream));
    if (result != Z_OK) {
        return result;
    }
    result = inflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        inflateEnd(&stream);
        return result == Z_OK ? Z_BUF_ERROR : result;
    }
    *destSize = static_cast<uint32_t>(stream.total_out);
    return inflateEnd(&stream);
}

// One permute of a loaded blob: its bytes are spread over 16 KB blocks, compressed unless a
// block is a full 16 KB, and come out 16-byte aligned in `buffer`.
// ref: FUN_1422948d0
const GXSHADERPERMUTE* ShaderPermute(uint8_t* blob, int32_t id, uint16_t permute, GXSHADERBUFFER* buffer) {
    auto header = reinterpret_cast<uint32_t*>(blob);
    auto permutes = reinterpret_cast<uint32_t*>(blob + header[2]);
    auto blocks = reinterpret_cast<uint32_t*>(blob + header[4]);
    uint8_t* data = blob + header[6];
    if (permute >= header[3]) {
        return nullptr;
    }
    uint32_t location = permutes[permute * 2];
    uint32_t size = permutes[permute * 2 + 1];
    if (size == 0) {
        char name[0x100];
        GxShaderName(id, name, sizeof(name));
        GxLog("Attempting to load deleted shader permute (%s:%hu)", name, permute);
        return &s_deletedPermute;
    }
    buffer->m_size = 0;
    buffer->resize(size + 0xf);
    auto out = reinterpret_cast<uint8_t*>((reinterpret_cast<uint64_t>(buffer->m_data) + 0xf) & ~0xfull);
    uint32_t block = location >> 0xe;
    uint32_t offset = location - block * 0x4000;
    uint8_t* write = out;
    if (offset != 0) {
        uint8_t scratch[0x4000];
        uint32_t begin = blocks[block];
        uint32_t end = offset + size;
        if (end > 0x4000) {
            end = 0x4000;
        }
        const uint8_t* source = data + begin;
        if (blocks[block + 1] - begin != 0x4000) {
            uint32_t inflated = end;
            InflateBlock(scratch, &inflated, source, blocks[block + 1] - begin);
            end = inflated;
            source = scratch;
        }
        std::memcpy(write, source + offset, end - offset);
        write += end - offset;
        size -= end - offset;
        block++;
    }
    while (size != 0) {
        uint32_t begin = blocks[block];
        uint32_t chunk = size > 0x4000 ? 0x4000 : size;
        if (blocks[block + 1] - begin == 0x4000) {
            std::memcpy(write, data + begin, 0x4000);
        } else {
            InflateBlock(write, &chunk, data + begin, blocks[block + 1] - begin);
        }
        block++;
        write += chunk;
        size -= chunk;
    }
    return reinterpret_cast<const GXSHADERPERMUTE*>(out);
}

// Loads every shader of the family's tables into the map.
void ShaderPreload(const int32_t* const* table, uint32_t count, int32_t family) {
    const int32_t* ids = table[family];
    if (ids == nullptr) {
        return;
    }
    for (uint32_t i = 1; i <= count; i++) {
        int32_t id = ids[i];
        uint8_t** slot = ShaderMapInsert(&s_shaders, id);
        ShaderLoad(slot, id, family);
    }
}

}

// ref: FUN_14230d6e0
const char* GxShaderName(int32_t id, char* buffer, uint32_t size) {
    SStrPrintf(buffer, size, "FileData ID %d", id);
    return buffer;
}

// ref: FUN_1422941d0
int32_t GxShaderVertexId(int32_t index) {
    if (s_family < 4 && s_vertexShaders[s_family] != nullptr && index < 0xaf) {
        return s_vertexShaders[s_family][index];
    }
    return 0;
}

// ref: FUN_142294140
int32_t GxShaderGeometryId(int32_t index) {
    if (s_family < 4 && s_geometryShaders[s_family] != nullptr && index < 7) {
        return s_geometryShaders[s_family][index];
    }
    return 0;
}

// ref: FUN_142294170
int32_t GxShaderPixelId(int32_t index) {
    if (s_family < 4 && s_pixelShaders[s_family] != nullptr && index < 0x117) {
        return s_pixelShaders[s_family][index];
    }
    return 0;
}

// ref: FUN_142294110
int32_t GxShaderComputeId(int32_t index) {
    if (s_family < 4 && s_computeShaders[s_family] != nullptr && index < 0x2a) {
        return s_computeShaders[s_family][index];
    }
    return 0;
}

// ref: FUN_142294200
void GxShaderSetFamily(const GXSHADERSETTINGS* settings) {
    bool changed = s_family != settings->m_family;
    s_loading = 1;
    if (changed && s_family != 4) {
        for (uint64_t i = 0; i < s_shaders.m_capacity; i++) {
            SHADERENTRY* entry = &s_shaders.m_entries[i];
            if (static_cast<uint32_t>(entry->m_id + 1) > 1 && entry->m_blob != nullptr) {
                SMemFree(entry->m_blob, s_smemNewFile, 0xa8, 0);
            }
            entry->m_id = 0;
        }
        s_shaders.m_size = 0;
        s_shaders.m_removed = 0;
        for (uint64_t i = 0; s_lateShaders.m_buckets != nullptr && i < s_lateShaders.m_bucketCount; i++) {
            SHADERNODE* node = s_lateShaders.m_buckets[i];
            while (node != nullptr) {
                SHADERNODE* next = node->m_next;
                if (node->m_blob != nullptr) {
                    SMemFree(node->m_blob, s_smemNewFile, 0xa8, 0);
                }
                SMemFree(node, s_lateNodeTag, -6, 0);
                node = next;
            }
            s_lateShaders.m_buckets[i] = nullptr;
        }
        s_lateShaders.m_size = 0;
    }
    s_shaders.m_maxLoad = 0.5f;
    s_cacheless = settings->m_cacheless;
    s_family = settings->m_family;
    if (s_cacheless == 0 && changed) {
        ShaderPreload(s_vertexShaders, 0xae, s_family);
        ShaderPreload(s_pixelShaders, 0x116, s_family);
        ShaderPreload(s_computeShaders, 0x29, s_family);
        ShaderPreload(s_geometryShaders, 6, s_family);
        // FUN_142293790: the two shaders of the extra table.
        if (s_extraShaders[s_family] != nullptr) {
            for (uint32_t i = 1; i <= 2; i++) {
                int32_t id = s_extraShaders[s_family][i];
                ShaderLoad(ShaderMapInsert(&s_shaders, id), id, s_family);
            }
        }
    }
    s_loading = 0;
}

// ref: FUN_142293cb0
const GXSHADERPERMUTE* GxShaderGetPermute(int32_t id, uint16_t permute, GXSHADERBUFFER* buffer) {
    uint8_t* loaded = nullptr;
    uint8_t** slot = nullptr;
    if (s_cacheless == 0) {
        slot = ShaderMapFind(&s_shaders, id);
        if (slot == nullptr) {
            AcquireSRWLockExclusive(&s_lateLock);
            bool inserted = false;
            slot = LateMapInsert(id, &inserted);
            if (inserted) {
                ShaderLoad(slot, id, s_family);
            }
            ReleaseSRWLockExclusive(&s_lateLock);
        }
    } else if (id != 0 && cas::CASFileExists(id)) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!cas::CASFileLoadById(id, &data, &size, 0, 0xd)) {
            if (s_loading == 0) {
                char message[0x100];
                cas::CASFormatLastError(message, sizeof(message));
                ShaderLog("Error Loading Shader: %s. Error Message: %s", id, message);
            }
        } else {
            // The reference reads permutes from the blob whether or not its header checked out.
            loaded = data;
            if (!ShaderExtractFamily(&loaded, size, s_family) && s_loading == 0) {
                ShaderLog("Error Loading Shader: %s", id);
            }
        }
        slot = &loaded;
    }
    const GXSHADERPERMUTE* result = nullptr;
    if (slot != nullptr && *slot != nullptr) {
        result = ShaderPermute(*slot, id, permute, buffer);
    } else {
        char name[0x100];
        GxShaderName(id, name, sizeof(name));
        GxLog("Error Processing Shader: %s", name);
        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Error Processing Shader: %s", name);
    }
    if (loaded != nullptr) {
        SMemFree(loaded, s_smemNewFile, 0xa8, 0);
    }
    return result;
}

// ---- the shader service ------------------------------------------------------------------

namespace {

// The DXR shader compiler service (vtable 0x1429196f0): dxcompiler for the ray tracing
// shaders, with a per-frame list of compile results.
struct RAYTRACINGSERVICE {
    HMODULE m_dxcompiler;           // 0x08
    void* m_createInstance;         // 0x10
    uint64_t m_frameCount;          // 0x100: the per-frame vector's size
};
RAYTRACINGSERVICE s_rayTracing;
bool s_rayTracingActive;            // PTR_PTR_142d43930 points at it, not the no-op service

}

// ref: FUN_1422952c0 (slot 0xb0 of the service: FUN_1422c1000 for ray tracing, a no-op else)
void GxShaderServiceStart() {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    uint8_t api = device != nullptr ? device->m_api : GxGetApi();
    if (api == GXAPI_D3D12 && reinterpret_cast<uint8_t*>(device)[0x12f] != 0) {
        s_rayTracingActive = true;
        HMODULE module = LoadLibraryW(L"dxcompiler.dll");
        HMODULE old = s_rayTracing.m_dxcompiler;
        s_rayTracing.m_dxcompiler = module;
        if (old != nullptr) {
            FreeLibrary(old);
        }
        if (s_rayTracing.m_dxcompiler != nullptr) {
            s_rayTracing.m_createInstance = reinterpret_cast<void*>(GetProcAddress(s_rayTracing.m_dxcompiler, "DxcCreateInstance"));
        }
        s_rayTracing.m_frameCount = device->FrameCount();
    }
}

// ref: FUN_142295310 (slot 0xb8: FUN_1422c1130 for ray tracing)
void GxShaderServiceStop() {
    if (s_rayTracingActive) {
        FROST_UNIMPLEMENTED();  // FUN_1422c1130: the ray tracing pipelines and compile results
        s_rayTracing.m_frameCount = 0;
    }
    s_rayTracingActive = false;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_143490a10 = (void*)&s_lateLock;
// ---- end of generated globals ----
