#include "world/WorldSceneRender.hpp"

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldView.hpp"
#include <cstring>
#include <new>

namespace {

const char s_renderFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\WorldSceneRender.cpp";

void InitIdentity(float* matrix) {
    for (int32_t i = 0; i < 16; i++) {
        matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
}

void InitProjection(WORLDRENDERPROJECTION* projection) {
    projection->m_00 = nullptr;
    projection->m_08 = 0;
    projection->m_14 = 0x19;
    projection->m_10 = 3;
    InitIdentity(projection->m_matrix);
    InitIdentity(projection->m_matrix2);
    projection->m_9c = 0;
    projection->m_rotation[0] = 0.0f;
    projection->m_rotation[1] = 0.0f;
    projection->m_rotation[2] = 0.0f;
    projection->m_rotation[3] = 1.0f;
    projection->m_b0 = 0;
    projection->m_b4 = 3;
    projection->m_b8 = 0;
    projection->m_ba = 0;
}

// The render's device-loss callback (LAB_1413ceae0).
void OnDeviceDestroyed(void* param) {
    static_cast<CWorldSceneRender*>(param)->m_resolve.OnDeviceDestroyed();
}

}  // namespace

// ref: FUN_1412bc9b0
void WORLDRENDERRESOLVE::OnDeviceDestroyed() {
    this->m_00 = 0;
    GxRenderTargetsClear(&this->m_buffer.m_targets);
    this->m_temp.m_tex = nullptr;
    this->m_130 = 0;
    GxRenderTargetsClear(&this->m_buffer2.m_targets);
    this->m_230[0] = 0;
    this->m_230[1] = 0;
    this->m_230[2] = 0;
    this->m_230[3] = 0;
}

// The pass state (FUN_1413ce660).
// ref: FUN_1413ce660
static void InitPass(WORLDRENDERPASS* pass) {
    std::memset(pass->m_0c, 0, sizeof(pass->m_0c));
    std::memset(pass->m_44, 0, sizeof(pass->m_44));
    std::memset(pass->m_58, 0, sizeof(pass->m_58));
    std::memset(pass->m_6c, 0, sizeof(pass->m_6c));
    std::memset(pass->m_84, 0, sizeof(pass->m_84));
    std::memset(pass->m_98, 0, sizeof(pass->m_98));
    std::memset(pass->m_ac, 0, sizeof(pass->m_ac));
    std::memset(pass->m_d0, 0, sizeof(pass->m_d0));
    pass->m_3c[0] = 1.0f;
    pass->m_3c[1] = 0.0f;
    pass->m_50[0] = 1.0f;
    pass->m_50[1] = 0.0f;
    pass->m_64[0] = 1.0f;
    pass->m_64[1] = 0.0f;
    pass->m_78 = 1.0f;
    pass->m_7c[0] = 1.0f;
    pass->m_7c[1] = 0.0f;
    pass->m_90[0] = 1.0f;
    pass->m_90[1] = 0.0f;
    pass->m_a4[0] = 1.0f;
    pass->m_a4[1] = 0.0f;
    pass->m_b8 = 1.0f;
}

// ref: FUN_1413cdca0
CWorldSceneRender::CWorldSceneRender() {
    InitIdentity(this->m_matrix);
    FrameBufferInit(&this->m_buffer070);
    FrameBufferInit(&this->m_buffer160);
    FrameBufferTempClear(&this->m_temp2b0);
    FrameBufferInit(&this->m_buffer2e8);
    this->m_3e8 = 0;
    for (FRAMEBUFFER& buffer : this->m_buffers3f0) {
        FrameBufferInit(&buffer);
    }
    InitPass(&this->m_pass);
    this->m_9d3 = 0;
    this->m_9d4 = 0;
    FrameBufferTempClear(&this->m_temp9d8);
    FrameBufferInit(&this->m_bufferA10);
    this->m_b00 = 0;
    this->m_b08 = 0;
    this->m_b10 = 0;
    this->m_b18 = 0;
    for (FRAMEBUFFERTEMP& temp : this->m_tempsB20) {
        FrameBufferTempClear(&temp);
    }
    FrameBufferInit(&this->m_bufferC00);
    FrameBufferInit(&this->m_resolve.m_buffer);
    FrameBufferTempClear(&this->m_resolve.m_temp);
    this->m_resolve.m_138 = 0;
    FrameBufferInit(&this->m_resolve.m_buffer2);
    this->m_f40 = 0;
    std::memset(this->m_f50, 0, sizeof(this->m_f50));
    this->m_group = nullptr;
    for (uint32_t i = 0; i < BUFFER_COUNT; i++) {
        this->m_buffers[i] = nullptr;
        this->m_contexts[i] = nullptr;
    }
    for (WORLDRENDERPROJECTION& projection : this->m_projections) {
        InitProjection(&projection);
    }
    this->m_13f0 = 0;
    this->m_13f8 = 0;
    this->m_1400 = 0.7518797f;  // 0x3f407b30
    this->m_1404 = 0;
    this->m_1408 = 0;
    this->m_040[0] = 0;
    this->m_040[1] = 0;
    this->m_040[2] = 0;
    this->m_040[3] = 0;
    this->m_9a8 = 0;
    this->m_9b0 = 0;
    this->m_9b8 = 0;
    void* memory = SMemAlloc(sizeof(ShaderEffectContext), 0x27, s_renderFile, SStrHashFnv1a(s_renderFile), 0x127, 0);
    this->m_effectContext = new (memory) ShaderEffectContext();
    this->m_group = ThreadPoolGroupCreate(2);
    for (uint32_t i = 0; i < BUFFER_COUNT; i++) {
        void* buffer = this->m_buffers[i];
        this->m_buffers[i] = nullptr;
        if (buffer != nullptr) {
            GxDeviceRelease098(&this->m_buffers[i], buffer);
        }
        this->m_contexts[i] = nullptr;
    }
    GxAddDeviceDestroyedCallback(OnDeviceDestroyed, this);
}

// ref: FUN_1413cea70
void CWorldSceneRender::ReleaseBuffers() {
    for (uint32_t i = 0; i < BUFFER_COUNT; i++) {
        void* buffer = this->m_buffers[i];
        this->m_buffers[i] = nullptr;
        if (buffer != nullptr) {
            GxDeviceRelease098(&this->m_buffers[i], buffer);
        }
        if (this->m_contexts[i] != nullptr) {
            operator delete(this->m_contexts[i]);
            this->m_contexts[i] = nullptr;
        }
    }
}

// ref: FUN_1413ce740
CWorldSceneRender::~CWorldSceneRender() {
    if (this->m_group != nullptr) {
        ThreadPoolGroupRelease(this->m_group);
        this->m_group = nullptr;
    }
    GxRemoveDeviceDestroyedCallback(OnDeviceDestroyed, this);
    this->m_resolve.OnDeviceDestroyed();
    this->ReleaseBuffers();
    operator delete(this->m_effectContext);
    for (uint32_t i = BUFFER_COUNT; i-- > 0;) {
        if (this->m_buffers[i] != nullptr) {
            GxDeviceRelease098(&this->m_buffers[i], this->m_buffers[i]);
        }
    }
}

namespace {

// The map's render mode (its definition's +0x100, 2 without one).
// ref: FUN_1413aed90
uint32_t WorldMapRenderMode(CWorldMap* map) {
    auto definition = map->Field<uint8_t*>(0x28);
    if (definition != nullptr) {
        return *reinterpret_cast<uint32_t*>(definition + 0x100);
    }
    return 2;
}

// The effect context's second-map factor (+0x614).
// ref: FUN_1412ad710
void ShaderEffectSetSecondMap(ShaderEffectContext* context, bool second) {
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(context) + 0x614) = second ? 1.0f : 0.0f;
}

bool IsWorldPass(CWorldSceneRender* render) {
    auto view = reinterpret_cast<CWorldView*>(render->m_040[1]);
    auto scene = reinterpret_cast<CWorldScene*>(render->m_040[0]);
    return view != nullptr && (view->m_flags & 2) == 0 && scene->m_type == 6;
}

}  // namespace

// ref: FUN_1413d8ac0
void CWorldSceneRender::Begin(CWorldScene* scene, CWorldView* view, WORLDRENDERPARAMS* params) {
    this->m_040[0] = reinterpret_cast<uint64_t>(scene);
    this->m_040[1] = reinterpret_cast<uint64_t>(view);
    this->m_040[2] = reinterpret_cast<uint64_t>(scene->m_maps[scene->m_mapIndex]);
    this->m_040[3] = reinterpret_cast<uint64_t>(params->m_m2Scene);
    auto fields = reinterpret_cast<uint8_t*>(this);
    *reinterpret_cast<FRAMEBUFFER**>(fields + 0x60) = params->m_scene;
    GXRENDERTARGETS* shared = nullptr;
    if (params->m_shared != nullptr && params->m_shared->m_targets.m_count != 0) {
        shared = &params->m_shared->m_targets;
    }
    *reinterpret_cast<GXRENDERTARGETS**>(fields + 0x68) = shared;
    *reinterpret_cast<uint32_t*>(fields + 0x9c8) = params->m_passes;
    std::memcpy(fields + 0x3d8, params->m_10, 0x10);
    std::memcpy(&this->m_3e8, params->m_size, 8);
    this->m_9a8 = reinterpret_cast<uint64_t>(scene->m_030);
    this->m_9b0 = reinterpret_cast<uint64_t>(scene->m_038);
    this->m_9b8 = reinterpret_cast<uint64_t>(scene->m_dayNight);
    MatrixIdentity(this->m_matrix);
    const float* position = view->m_camera.m_position;
    float move[3] = {-position[0], -position[1], -position[2]};
    MatrixTranslate(this->m_matrix, move);
    *reinterpret_cast<uint32_t*>(fields + 0x9cc) = WorldMapRenderMode(reinterpret_cast<CWorldMap*>(this->m_040[2]));
    fields[0x9d0] = params->m_80[0];
    fields[0x9d1] = params->m_80[1];
    this->m_9d3 = params->m_80[3];
    bool rayTracing = params->m_80[2] != 0 && GxCurrentApi() != 0 && (g_renderSettings.m_flags & 0x8000) != 0;
    fields[0x9d2] = rayTracing ? 1 : 0;
    ShaderEffectSetSecondMap(this->m_effectContext, static_cast<uint8_t>(scene->m_680) == 2);
}


// Only the world's own scene (type 6) traces shadows; its targets are not ported.
// ref: FUN_1413ceb50
void CWorldSceneRender::SetupRayTracing(bool rayTracing, FRAMEBUFFERTEMP* temp, void** objects) {
    (void)rayTracing;
    (void)temp;
    (void)objects;
    if (!IsWorldPass(this)) {
        return;
    }
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413d8c80
void CWorldSceneRender::DispatchRayTracing(bool rayTracing) {
    (void)rayTracing;
    if (!IsWorldPass(this)) {
        return;
    }
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412b8160
void WORLDRENDERRESOLVE::Release() {
    FrameBufferRelease(&this->m_buffer);
    if (this->m_temp.m_tex != nullptr) {
        FrameBufferTempRelease(&this->m_temp);
    }
    if (this->m_buffer2.m_targets.m_count != 0) {
        FrameBufferRelease(&this->m_buffer2);
    }
}
