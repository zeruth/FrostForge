#include "world/WorldView.hpp"

#include "model2/M2Shared.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "threadpool/ThreadPool.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include "world/CullWorker.hpp"
#include "world/SceneOcclusion.hpp"
#include "world/WorldClip.hpp"
#include "world/WorldQueries.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldSceneRender.hpp"
#include <cstring>
#include <new>

namespace {

const char s_viewFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\WorldView.cpp";

void* ViewAlloc(size_t size, int32_t line) {
    return SMemAlloc(size, 0x27, s_viewFile, SStrHashFnv1a(s_viewFile), line, 0);
}

void InitCamera(WORLDVIEWCAMERA* camera) {
    for (int32_t i = 0; i < 3; i++) {
        camera->m_position[i] = 0.0f;
        camera->m_target[i] = 0.0f;
    }
    camera->m_nearClip = 0.2f;
    camera->m_farClip = 4400.0f;
    camera->m_farClip2 = 4400.0f;
    camera->m_24 = 1300.0f;
    camera->m_fov = 0.9424778f;     // 0x3f71463a
    camera->m_aspect = 1.7777778f;  // 0x3fe38e39
}

}  // namespace

// ref: FUN_1413d9420
CWorldView::CWorldView(CWorldScene* scene, CWorldView* parent, uint32_t mode) {
    this->m_04c = 0x19;
    this->m_048 = 3;
    this->m_038 = nullptr;
    this->m_040 = nullptr;
    std::memset(this->m_050, 0, sizeof(this->m_050));
    std::memset(this->m_110, 0, sizeof(this->m_110));
    this->m_230 = 0;
    for (WORLDVIEWGATHER& gather : this->m_gathered) {
        gather.m_00 = nullptr;
        gather.m_08 = 0;
        gather.m_1c = 0;
        gather.m_20 = 0;
    }
    this->m_2e8 = 0;
    this->m_2ec = 0;
    this->m_2f0 = 0;
    this->m_occlusion = nullptr;
    this->m_300 = 0;
    this->m_308 = 0;
    std::memset(this->m_30c, 0, sizeof(this->m_30c));
    for (auto& plane : this->m_planes) {
        plane[0] = 0.0f;
        plane[1] = 0.0f;
        plane[2] = 0.0f;
        plane[3] = 1.0f;
    }
    std::memset(this->m_3f0, 0, sizeof(this->m_3f0));
    this->m_targetSize[0] = 0;
    this->m_targetSize[1] = 0;
    InitCamera(&this->m_cameraSet);
    InitCamera(&this->m_cameraLast);
    InitCamera(&this->m_camera);
    for (auto& matrix : this->m_matrices) {
        for (int32_t i = 0; i < 16; i++) {
            matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        }
    }
    this->m_clipPlane[0] = 0.0f;
    this->m_clipPlane[1] = 0.0f;
    this->m_clipPlane[2] = 1.0f;
    this->m_clipPlane[3] = 0.0f;
    for (WORLDVIEWLIST& list : this->m_lists) {
        OsMutexInit(&list.m_lock);
        list.m_linkoffset = 0;
        list.m_head[0] = &list.m_head[0];
        list.m_head[1] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&list.m_head[0]) | 1);
        list.m_group = nullptr;
        list.m_28 = 0;
        list.m_held = 0;
        list.m_2c = 0;
    }

    this->m_flags = mode;
    uint32_t parentIndex = 0;
    if (scene->m_view != nullptr) {
        parentIndex = scene->m_view->m_index;
    }
    this->m_renderIndex = static_cast<uint32_t>(scene->m_renders.size());
    this->m_parent = parent;
    this->m_index = this->m_renderIndex + parentIndex;
    this->m_scene = scene;
    void* memory = ViewAlloc(sizeof(CWorldSceneRender), 0x70);
    this->m_render = new (memory) CWorldSceneRender();
    this->m_scene->m_renders.push_back(this->m_render);
    if (this->m_parent != nullptr) {
        uint8_t context[ShaderEffectContext::SIZE];
        std::memcpy(context, this->m_parent->m_render->m_effectContext, sizeof(context));
        std::memcpy(this->m_render->m_effectContext, context, sizeof(context));
    }
    this->m_children[0] = nullptr;
    this->m_children[1] = nullptr;
    this->m_230 = 0;
    this->m_238 = nullptr;
    this->m_cameraSet.m_farClip = WorldGetFarClipCVar();
    this->m_cameraSet.m_nearClip = WorldGetNearClipCVar();
    this->m_cameraSet.m_24 = WorldGetFarClip3CVar();
    this->m_cameraSet.m_fov = 0.9424778f;
    this->m_cameraSet.m_aspect = 1.7777778f;
    memory = ViewAlloc(sizeof(CWorldCull), 0x83);
    this->m_cull = new (memory) CWorldCull(this);
    this->m_04c = (mode & 2) != 0 ? 0x14 : 6;
    if ((this->m_flags & 4) == 0) {
        this->m_048 = ((this->m_flags & 0xff) >> 1) & 1;
    } else {
        this->m_048 = 2;
    }
    this->m_038 = scene;
    this->m_040 = this;
    this->m_690 = 0;
    this->m_694 = 0;
    this->m_698 = 0;
    this->m_69c = 0;
    this->m_6a0 = 0;
    this->m_6b4 = 4;
    this->m_scale[0] = 1.0f;
    this->m_scale[1] = 1.0f;
    this->m_scale[2] = 1.0f;
    SceneOcclusionCreate(&this->m_occlusion);
}

// ref: FUN_1413dd0a0
void* WorldViewAddProjected(CWorldView* view, void* query) {
    if (view->m_lists[1].m_held != 0) {
        return nullptr;
    }
    void* decal = WorldProjectedDecalCreate(query);
    if (decal != nullptr) {
        WorldViewListAdd(&view->m_lists[1], decal);
    }
    return decal;
}

// ref: FUN_1413da150
void WorldViewAddPhysics(CWorldView* view, uint8_t* state) {
    view->m_cull->AddPhysics(state);
}

// ref: FUN_1413d9ed0
CWorldView::~CWorldView() {
    this->m_flags &= ~0x400u;
    if (this->m_238 != nullptr) {
        WorldViewReflectionDestroy(this->m_238);
        M2FreeFileData(this->m_238);
        this->m_238 = nullptr;
    }
    for (WORLDVIEWLIST& list : this->m_lists) {
        WorldViewListClear(&list);
    }
    if (this->m_cull != nullptr) {
        delete this->m_cull;
        this->m_cull = nullptr;
    }
    if (this->m_render != nullptr) {
        auto& renders = this->m_scene->m_renders;
        uint64_t kept = 0;
        for (uint64_t i = 0; i < renders.size(); i++) {
            if (renders[i] != this->m_render) {
                renders[kept++] = renders[i];
            }
        }
        // The reference compacts in place without shrinking the count (std::remove).
        delete this->m_render;
        this->m_render = nullptr;
    }
    if (this->m_children[0] != nullptr) {
        delete this->m_children[0];
        this->m_children[0] = nullptr;
    }
    if (this->m_children[1] != nullptr) {
        delete this->m_children[1];
        this->m_children[1] = nullptr;
    }
    SceneOcclusionDestroy(&this->m_occlusion);
    WorldViewListDestroy(&this->m_lists[1]);
    WorldViewListDestroy(&this->m_lists[0]);
}

// ref: FUN_1413a2ad0
void WorldViewListClear(WORLDVIEWLIST* list) {
    uintptr_t head = reinterpret_cast<uintptr_t>(list->m_head[1]);
    if ((head & 1) == 0 && head != 0) {
        if (list->m_2c != list->m_held && list->m_held != 0) {
            ThreadPoolGroupWait(list->m_group);
            list->m_2c = list->m_held;
        }
        uintptr_t node = reinterpret_cast<uintptr_t>(list->m_head[1]);
        while ((node & 1) == 0 && node != 0) {
            uintptr_t next = *reinterpret_cast<uintptr_t*>(node + list->m_linkoffset + 8);
            WorldViewListRelease(reinterpret_cast<void*>(node));
            node = next;
        }
    }
    list->m_28 = 0;
    list->m_held = 0;
    list->m_2c = 0;
}

// ref: FUN_141364bb0
void WorldViewListDestroy(WORLDVIEWLIST* list) {
    if (list->m_group != nullptr) {
        ThreadPoolGroupRelease(list->m_group);
        list->m_group = nullptr;
    }
    auto terminator = reinterpret_cast<TSLink<uint8_t>*>(&list->m_head[0]);
    while (uint8_t* node = terminator->Next()) {
        reinterpret_cast<TSLink<uint8_t>*>(node + list->m_linkoffset)->Unlink();
    }
    terminator->Unlink();
}

// ref: FUN_141351b90
uint32_t CWorldView::GetLiquid(float* depth) {
    if (depth != nullptr) {
        std::memcpy(depth, &this->m_694, 4);
    }
    return this->m_690;
}

// ref: FUN_1413dd190
int32_t CWorldView::QueryWater(uint8_t* fog, uint8_t* covered, float* depth) {
    (void)fog;
    (void)covered;
    (void)depth;
    auto chunk = static_cast<uint8_t*>(this->m_gathered[0].m_00);
    if (chunk == nullptr) {
        return 0;
    }
    if (*reinterpret_cast<void**>(chunk + 0x58) == nullptr) {
        return 0;
    }
    FROST_UNIMPLEMENTED();  // the chunk's liquids (FUN_141383ba0, FUN_1413e40f0) and the water fog (FUN_1413dcc80, FUN_1413da900)
    return 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141383ba0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413da900=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413dcc80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413e40f0=GenForwardMissing")
// ---- end of link fallbacks ----
