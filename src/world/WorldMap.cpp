#include "world/WorldMap.hpp"

#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/list/TSList.hpp"
#include "threadpool/ThreadPool.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldScene.hpp"
#include <windows.h>
#include <cstring>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

namespace {

const char s_mapFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\WorldMap.cpp";

// An embedded TSList: its link offset, then its terminator (the previous link pointing at
// itself, the next one at itself tagged as the end).
void InitList(CWorldMap* map, size_t offset, uint32_t linkOffset) {
    map->Field<uint32_t>(offset) = linkOffset;
    auto terminator = reinterpret_cast<uintptr_t>(map->m_data + offset + 8);
    map->Field<uintptr_t>(offset + 8) = terminator;
    map->Field<uintptr_t>(offset + 0x10) = terminator | 1;
}

}  // namespace

// The map's 0x28 load state (FUN_142474960, reset by FUN_142474b40): no request, the slot
// -1, idle.
// ref: FUN_142474960
void MapLoadStateInit(uint8_t* state) {
    *reinterpret_cast<uint64_t*>(state + 0x10) = 0xffffffffffffffffull;
    *reinterpret_cast<uint64_t*>(state) = 0;
    *reinterpret_cast<uint64_t*>(state + 8) = 0;
    *reinterpret_cast<uint64_t*>(state + 0x18) = 0;
    state[0x20] = 1;
    InterlockedExchange(reinterpret_cast<volatile long*>(state + 0x24), 0);
}


// ref: FUN_142474b40
void MapLoadStateReset(uint8_t* state) {
    MapLoadStateInit(state);
}

namespace {

// ref: FUN_14142b2e0
uint8_t* MapLoadStateCreate(uint8_t* state) {
    MapLoadStateInit(state);
    MapLoadStateReset(state);
    return state;
}

// DAT_142d01200: every map, linked at the tail as it is made (the link is the map's first
// field).
TSList<CWorldMap, TSGetLink<CWorldMap>, 8> s_maps;

}  // namespace

// The hash tables' and lists' links are set up as the reference sets them. The three hash
// tables' vtables are the reference's own in the mapped image: their methods are not ported,
// and the image's entries run their translations (the map loader, translated, calls them).
// ref: FUN_1413aab10
CWorldMap::CWorldMap(CWorldScene* scene, uint32_t slot) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    this->Field<uintptr_t>(0x128) = 0x142738460;
    this->Field<uintptr_t>(0x178) = 0x142738480;
    this->Field<uintptr_t>(0x2a0) = 0x1427384a0;
    // The doodad hash table (+0x128): its list (link 0xc0) and its arrays.
    InitList(this, 0x130, 0xc0);
    this->Field<uint32_t>(0x170) = 0xffffffff;
    this->Field<uint32_t>(0x174) = 0x80000000;
    // The map object hash table (+0x178).
    InitList(this, 0x180, 0x28);
    this->Field<uint32_t>(0x1c0) = 0xffffffff;
    this->Field<uint32_t>(0x1c4) = 0x80000000;
    InitList(this, 0x1c8, 0xb0);
    this->Field<uint8_t>(0x1e8) = 1;
    InitList(this, 0x1f0, 0x38);
    InitList(this, 0x208, 0x60);
    InitList(this, 0x220, 0x70);
    InitList(this, 0x238, 0x28);
    InitList(this, 0x270, 0x88);
    InitList(this, 0x288, 0xb0);
    // The third hash table (+0x2a0).
    InitList(this, 0x2a8, 0x10);
    this->Field<uint32_t>(0x2e8) = 0xffffffff;
    this->Field<uint32_t>(0x2ec) = 0x80000000;
    // blz::unordered_map at +0x2f0: eight buckets, load limit 1.
    this->Field<uint64_t>(0x2f0) = 8;
    this->Field<float>(0x308) = 1.0f;
    InitList(this, 0x450, 0x138);
    InitList(this, 0x468, 0x138);
    InitList(this, 0x480, 0x498);
    OsMutexInit(reinterpret_cast<OSMUTEX*>(this->m_data + 0x4b8));
    this->Field<CWorldScene*>(0x10) = scene;
    this->Field<float>(0x4d8) = 19.0f;
    this->Field<float>(0x4dc) = 30.0f;
    this->Field<float>(0x4e0) = 60.0f;
    this->Field<float>(0x4e4) = 90.0f;
    this->Field<float>(0x4e8) = 1.0f;
    this->Field<float>(0x4ec) = 1.0f;
    this->Field<float>(0x4f0) = 1.0f;
    this->Field<float>(0x4f4) = 1.0f;
    this->Field<float>(0x4f8) = 1.0f;
    this->Field<float>(0x500) = 1.0f;
    this->Field<float>(0x504) = 1.0f;
    this->Field<uint8_t>(0x24) &= 0x40;
    this->Field<uint8_t>(0x25) = static_cast<uint8_t>((this->Field<uint8_t>(0x25) & 0xe0) | 0x20);
    this->Field<uint64_t>(0x18) = 0xffffffffffffffffull;
    this->Field<uint32_t>(0x20) = slot;
    this->Field<uint32_t>(0x268) = 0x80;
    this->Field<THREADPOOLGROUP*>(0x4c0) = ThreadPoolGroupCreate(2);
    this->Field<THREADPOOLGROUP*>(0x4c8) = ThreadPoolGroupCreate(2);
    this->Field<THREADPOOLGROUP*>(0x4d0) = ThreadPoolGroupCreate(2);
}

extern "C" {
unsigned long long FUN_1402f9a70(unsigned long long);   // a TSList's destructor: every node and the terminator unlinked
unsigned long long FUN_1413ab570(unsigned long long);
unsigned long long FUN_1413ab4f0(unsigned long long);
unsigned long long FUN_1413ab470(unsigned long long);
unsigned long long FUN_1413ab3f0(unsigned long long);
}

namespace {

const char s_mapArrayTag[] = ".?AV?$TSFixedArray@VCMapTableEntry@@$07@@";

// The members' teardown, by the reference's offsets (the map is raw reference bytes): the two
// world query vectors (VolumeIntersectResult at +0x508, OnFacetQueryValidCallbackData at +0x498),
// the lists, the sub-objects' own destructors (translated) and the two fixed arrays.
void WorldMapFreeMembers(uint8_t* map) {
    auto q = reinterpret_cast<uint64_t*>(map);
    auto list = [map](uint32_t qword) {
        FUN_1402f9a70(reinterpret_cast<unsigned long long>(map + qword * 8));
    };
    __try {
        if (static_cast<int64_t>(q[0xa3]) >= 0 && q[0xa1] != 0) {
            SMemFree(reinterpret_cast<void*>(q[0xa1]), s_mapArrayTag, -6, 0);
            q[0xa1] = 0;
        }
        if (static_cast<int64_t>(q[0x95]) >= 0 && q[0x93] != 0) {
            SMemFree(reinterpret_cast<void*>(q[0x93]), s_mapArrayTag, -6, 0);
            q[0x93] = 0;
        }
        list(0x90);
        list(0x8d);
        list(0x8a);
        FUN_1413ab570(reinterpret_cast<unsigned long long>(map + 0x5e * 8));
        FUN_1413ab4f0(reinterpret_cast<unsigned long long>(map + 0x54 * 8));
        list(0x51);
        list(0x4e);
        if (q[0x4b] != 0) {
            SMemFree(reinterpret_cast<void*>(q[0x4b]), s_mapArrayTag, -2, 0);
        }
        list(0x47);
        list(0x44);
        list(0x41);
        list(0x3e);
        list(0x39);
        FUN_1413ab470(reinterpret_cast<unsigned long long>(map + 0x2f * 8));
        FUN_1413ab3f0(reinterpret_cast<unsigned long long>(map + 0x25 * 8));
        if (q[0x22] != 0) {
            SMemFree(reinterpret_cast<void*>(q[0x22]), s_mapArrayTag, -2, 0);
        }
    } __except (GenFault(0x1413ab600ull, GetExceptionInformation())) {
    }
}

}  // namespace

// ref: FUN_1413ab600
CWorldMap::~CWorldMap() {
    WorldMapFreeMembers(this->m_data);
    // the last step: leaving every map
    s_maps.Link(this)->Unlink();
}

extern "C" unsigned long long FUN_141427730(unsigned long long, unsigned long long);
// translated: FUN_141427730
void CWorldMap::SetCameraPosition(const float* position) {
    __try {
        FUN_141427730((unsigned long long)this, (unsigned long long)position);
    } __except (GenFault(0x141427730ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1413ad010
void CWorldMap::LoadNone(int32_t mapId, bool second) {
    this->Field<int32_t>(0x18) = mapId;
    this->Field<uint8_t>(0x24) = static_cast<uint8_t>((this->Field<uint8_t>(0x24) & 0xdf) | (second ? 0x20 : 0));
    if (this->Field<uint32_t>(0x20) == 0) {
        this->Scene()->CreateMapObjects();
    }
    this->LoadStaticObjects(false, -1);
    this->CreateLoadState();
}

// The map's static collision and its objects from the static tables and the DB2 rows that
// name the map (or `alsoMapId` when `also`).
extern "C" unsigned long long FUN_14142a610(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_14142a610
void CWorldMap::LoadStaticObjects(bool also, int32_t alsoMapId) {
    __try {
        FUN_14142a610((unsigned long long)this, (unsigned long long)also, (unsigned long long)alsoMapId);
    } __except (GenFault(0x14142a610ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1413b0e40
void CWorldMap::CreateLoadState() {
    auto state = static_cast<uint8_t*>(SMemAlloc(0x28, 0x27, s_mapFile, SStrHashFnv1a(s_mapFile), 0x1fd, 0));
    this->Field<uint8_t*>(0x1e0) = MapLoadStateCreate(state);
}

extern "C" unsigned long long FUN_1413b0980(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1413b0980
void CWorldMap::Load(int32_t mapId, uint8_t a3, int32_t a4, uint8_t a5) {
    MemoryGuardStart();  // FROST_GUARD_HEAP=world only: past the Battle.net login from here
    __try {
        FUN_1413b0980((unsigned long long)this, (unsigned long long)mapId, (unsigned long long)a3, (unsigned long long)a4, (unsigned long long)a5);
    } __except (GenFault(0x1413b0980ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1413b1f60(unsigned long long);
// translated: FUN_1413b1f60
void CWorldMap::Unload() {
    __try {
        FUN_1413b1f60((unsigned long long)this);
    } __except (GenFault(0x1413b1f60ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1413cac10
void WorldMapLink(CWorldMap* map) {
    s_maps.Link(map)->Unlink();
    s_maps.LinkToTail(map);
}

extern "C" unsigned long long FUN_14142d0c0(unsigned long long, unsigned long long);
extern "C" unsigned long long FUN_141427610(unsigned long long);
// translated: FUN_14142d0c0
// translated: FUN_141427610
// An area no one holds any more: its light leaves the map's light set, it lets go of what it
// loaded, and it is deleted.
// ref: FUN_1413b2320
void CWorldMap::ReleaseArea(void* area) {
    void* lights = this->Field<void*>(0xe8);
    if (lights != nullptr) {
        FUN_14142d0c0(reinterpret_cast<uintptr_t>(lights), *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(area) + 0xfc));
    }
    FUN_141427610(reinterpret_cast<uintptr_t>(area));
    if (area != nullptr) {
        auto destroy = **reinterpret_cast<void (***)(void*, int32_t)>(area);
        destroy(area, 1);
    }
}

// Once a frame (when asked): every area whose reference count reached 0 is released.
// ref: FUN_1413ae6d0
void CWorldMap::ReleaseUnusedAreas(bool release) {
    if (!release) {
        return;
    }
    for (int32_t i = 0; i < 0x18; i++) {
        void*& area = this->Field<void*>(0x28 + i * 8);
        if (area != nullptr && *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(area) + 0xc) == 0) {
            this->ReleaseArea(area);
            area = nullptr;
        }
    }
}

// ref: FUN_1413cb0e0
void WorldMapsReleaseUnusedAreas(bool release) {
    for (CWorldMap* map = s_maps.Head(); map != nullptr; map = s_maps.Next(map)) {
        map->ReleaseUnusedAreas(release);
    }
}

// Whether the map's definition asks for a water clip plane (its +0x104 bit 6).
// ref: FUN_1413b7140
bool WorldMapHasWaterPlane(CWorldMap* map) {
    auto definition = map->Field<uint8_t*>(0x28);
    if (definition != nullptr) {
        return (definition[0x104] >> 6 & 1) != 0;
    }
    return false;
}

// The map's fading objects (the list at +0x2a0): each fades in (flag 0x20, up to 1) or out by
// the frame's time, and one faded out is dropped.
// ref: FUN_14142a3f0
void WorldMapUpdateFades(CWorldMap* map) {
    float elapsed = FrameTime_GetElapsedSeconds();
    int32_t linkOffset = map->Field<int32_t>(0x2a8);
    uintptr_t node = map->Field<uintptr_t>(0x2b8);
    while ((node & 1) == 0 && node != 0) {
        uintptr_t next = *reinterpret_cast<uintptr_t*>(node + linkOffset + 8);
        auto bytes = reinterpret_cast<uint8_t*>(node);
        uint16_t flags = *reinterpret_cast<uint16_t*>(bytes + 0x4a);
        float fade;
        if ((flags & 0x20) == 0) {
            flags &= 0xffbf;
            fade = *reinterpret_cast<float*>(bytes + 0x40) - elapsed;
        } else {
            fade = *reinterpret_cast<float*>(bytes + 0x40) + elapsed;
            flags |= 0x40;
            if (1.0f <= fade) {
                fade = 1.0f;
            }
        }
        *reinterpret_cast<float*>(bytes + 0x40) = fade;
        *reinterpret_cast<uint16_t*>(bytes + 0x4a) = static_cast<uint16_t>(flags & 0xffdf);
        if (fade <= 0.0f) {
            WorldMapRemoveFaded(map, bytes);
        }
        node = next;
    }
}

// Drops a faded object: its texture, its hash table entry, the object (FUN_141281690,
// FUN_140325650, FUN_1413e7630). Not ported.
extern "C" unsigned long long FUN_1413ae680(unsigned long long, unsigned long long);
// translated: FUN_1413ae680
void WorldMapRemoveFaded(CWorldMap* map, void* object) {
    __try {
        FUN_1413ae680((unsigned long long)map, (unsigned long long)object);
    } __except (GenFault(0x1413ae680ull, GetExceptionInformation())) {
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1402f9a70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ab3f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ab470=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ab4f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ab570=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ae680=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413b0980=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413b1f60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141427610=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141427730=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14142a610=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14142d0c0=GenForwardMissing")
// ---- end of link fallbacks ----
