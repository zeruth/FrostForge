#include "world/WorldMap.hpp"

#include "util/Unimplemented.hpp"
#include <cstring>

// The map is the next port (its constructor's hash tables, map -1's load and the light path it
// starts). Until then it is zeroed and does nothing.

// ref: FUN_1413aab10
CWorldMap::CWorldMap(CWorldScene* scene, uint32_t slot) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    this->Field<CWorldScene*>(0x10) = scene;
    this->Field<int32_t>(0x18) = -1;
    this->Field<uint32_t>(0x20) = slot;
    FROST_UNIMPLEMENTED();
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
    (void)mapId;
    (void)second;
    FROST_UNIMPLEMENTED();
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
