#include "world/WorldMap.hpp"

#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldScene.hpp"
#include <windows.h>
#include <cstring>

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

// ref: FUN_14142b2e0
uint8_t* MapLoadStateCreate(uint8_t* state) {
    MapLoadStateInit(state);
    MapLoadStateReset(state);
    return state;
}

}  // namespace

// The hash tables' and lists' links are set up as the reference sets them; the three hash
// tables' vtables (0x142738460, 0x142738480, 0x1427384a0) stay null until their types are
// ported with the map loader that fills them.
// ref: FUN_1413aab10
CWorldMap::CWorldMap(CWorldScene* scene, uint32_t slot) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
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

// ref: FUN_1413ab600
CWorldMap::~CWorldMap() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141427730
void CWorldMap::SetCameraPosition(const float* position) {
    (void)position;
    FROST_UNIMPLEMENTED();
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
// ref: FUN_14142a610
void CWorldMap::LoadStaticObjects(bool also, int32_t alsoMapId) {
    (void)also;
    (void)alsoMapId;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413b0e40
void CWorldMap::CreateLoadState() {
    auto state = static_cast<uint8_t*>(SMemAlloc(0x28, 0x27, s_mapFile, SStrHashFnv1a(s_mapFile), 0x1fd, 0));
    this->Field<uint8_t*>(0x1e0) = MapLoadStateCreate(state);
}

// ref: FUN_1413b0980
void CWorldMap::Load(int32_t mapId, uint8_t a3, int32_t a4, uint8_t a5) {
    (void)mapId;
    (void)a3;
    (void)a4;
    (void)a5;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413b1f60
void CWorldMap::Unload() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413cac10
void WorldMapLink(CWorldMap* map) {
    (void)map;
    FROST_UNIMPLEMENTED();
}
