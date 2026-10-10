#ifndef WORLD_WORLD_MAP_HPP
#define WORLD_WORLD_MAP_HPP

// WoW/Source/WorldClient2/WorldMap.cpp: CWorldMap (0x520 bytes), a scene's map. A model
// frame's scene loads map -1, which reads no terrain but still sets up the lighting and sky a
// map without data falls back to.

#include <cstddef>
#include <cstdint>

class CWorldScene;

class CWorldMap {
    public:
    uint8_t m_data[0x520];

    // ref: FUN_1413aab10
    CWorldMap(CWorldScene* scene, uint32_t slot);
    // ref: FUN_1413ab600
    ~CWorldMap();

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(this->m_data + offset);
    }

    CWorldScene* Scene() {
        return this->Field<CWorldScene*>(0x10);
    }
    int32_t MapId() {
        return this->Field<int32_t>(0x18);
    }
    // +0x24: bit 1 caps the far clip at 4400, bit 5 a second map.
    uint8_t Flags() {
        return this->Field<uint8_t>(0x24);
    }
    // +0x28: the areas (0x18 of them), each with a light id at +0xfc.
    void* Area(int32_t index) {
        return this->Field<void*>(0x28 + index * 8);
    }
    float* Position() {
        return &this->Field<float>(0x404);
    }
    float FarClipScale() {
        return this->Field<float>(0x4f4);
    }
    float FarClip2Scale() {
        return this->Field<float>(0x4f8);
    }

    // The camera position, the far clip it allows and the chunks around it.
    // ref: FUN_141427730
    void SetCameraPosition(const float* position);
    // Map `mapId` without terrain (what a model frame's scene loads).
    // ref: FUN_1413ad010
    void LoadNone(int32_t mapId, bool second);
    // ref: FUN_1413b0980
    void Load(int32_t mapId, uint8_t a3, int32_t a4, uint8_t a5);
    // ref: FUN_1413b1f60
    void Unload();
};
static_assert(sizeof(CWorldMap) == 0x520, "CWorldMap is 0x520 bytes");

// Links a map into every map (DAT_142d01200), at the tail.
// ref: FUN_1413cac10
void WorldMapLink(CWorldMap* map);

#endif
