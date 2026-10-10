#ifndef WORLD_SCENE_OCCLUSION_HPP
#define WORLD_SCENE_OCCLUSION_HPP

// Engine/Source/SceneOcclusion/SceneOcclusion.cpp: a view's occlusion (0x7160 bytes): sixteen
// occluder buffers (0x700 each, from +0x120), a lock and two thread pool groups. The view
// holds a pointer to it (its +0x2f8), which is what the SceneOcclusion calls take.

#include "os/Sync.hpp"
#include <cstddef>
#include <cstdint>

struct THREADPOOLGROUP;

class SceneOcclusion {
    public:
    enum : uint32_t {
        BUFFER_COUNT = 16,
        BUFFER_SIZE = 0x700,
    };

    uint8_t m_data[0x7160];

    // ref: FUN_142551700
    explicit SceneOcclusion(SceneOcclusion** holder);

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(this->m_data + offset);
    }

    // ref: FUN_142552520
    void Reset();
};

// One occluder buffer (FUN_142551810) and its rasterizer state at +0x260 (FUN_142553b50).
// ref: FUN_142551810
void SceneOcclusionBufferInit(uint8_t* buffer);
// ref: FUN_142553b50
void SceneOcclusionRasterInit(uint8_t* raster);

// Makes the view's occlusion (SceneOcclusion.cpp:0x18) and resets it.
// ref: FUN_14254fe70
void SceneOcclusionCreate(SceneOcclusion** holder);
// Waits for the occlusion's jobs, frees its buffers' meshes and it.
// ref: FUN_14254fd60
void SceneOcclusionDestroy(SceneOcclusion** holder);
// ref: FUN_142551ab0
void SceneOcclusionShutdown(SceneOcclusion* occlusion);
// The stage the occlusion is in: 0 idle, 5 done.
// ref: FUN_14254fe20
uint32_t SceneOcclusionGetState(SceneOcclusion** holder);

#endif
