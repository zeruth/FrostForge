// WoW/Source/WorldClient2/WorldSceneRender.cpp: a view's passes (CWorldSceneRender::Draw,
// FUN_1413cf390) — the M2 scene's frame state, the frame's environment constants and clear, the
// opaque pass (FUN_1413d47c0) and the transparent and composite pass (FUN_1413d07b0). Only the
// paths a frame takes without deferred command lists are ported: the reference records those
// lists only with ray tracing on (render +0x9d2), and the world's own scene (type 6) parts —
// terrain, map objects, liquids, ray tracing, the cascaded shadow maps and SSAO — are tagged
// stubs here.

#include "world/WorldSceneRender.hpp"
#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "daynight/DayNight.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "model2/M2Scene.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "texture/Texture.hpp"
#include "threadpool/ThreadPool.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include "world/CullWorker.hpp"
#include "world/SceneOcclusion.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldStubs.hpp"
#include "world/WorldFrame.hpp"
#include "world/WorldView.hpp"
#include <cstring>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

namespace {

template <class T>
T& At(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

CWorldScene* SceneOf(CWorldSceneRender* render) {
    return reinterpret_cast<CWorldScene*>(render->m_040[0]);
}

CWorldView* ViewOf(CWorldSceneRender* render) {
    return reinterpret_cast<CWorldView*>(render->m_040[1]);
}

CM2Scene* M2SceneOf(CWorldSceneRender* render) {
    return reinterpret_cast<CM2Scene*>(render->m_040[3]);
}

FRAMEBUFFER* SceneBufferOf(CWorldSceneRender* render) {
    return At<FRAMEBUFFER*>(render, 0x60);
}

GXRENDERTARGETS* TargetsOrNull(FRAMEBUFFER* buffer) {
    return buffer->m_targets.m_count != 0 ? &buffer->m_targets : nullptr;
}

DAYNIGHT* DayNightOf(CWorldSceneRender* render) {
    return reinterpret_cast<DAYNIGHT*>(render->m_9b8);
}

// The scene's view of weather: its direction through a matrix's rotation and translation.
void TransformPoint(const float* m, const float* v, float* out) {
    for (int32_t i = 0; i < 3; i++) {
        out[i] = v[1] * m[4 + i] + v[0] * m[i] + v[2] * m[8 + i] + m[12 + i];
    }
}

// ---- the M2 scene's frame state ------------------------------------------------------------

// ref: FUN_1411e0eb0
void M2SceneSetClipPlane(CM2Scene* scene, bool clip, const float* plane) {
    if (clip) {
        scene->m_flags |= 0x800;
    } else {
        scene->m_flags &= ~0x800u;
    }
    std::memcpy(&At<float>(scene, 0xd568), plane, 0x10);
}

// The M2 scene's jobs waited on, when it started them (+0xdb20, group +0xdb10).
// ref: FUN_1411e44a0
void M2SceneWaitJobs(CM2Scene* scene) {
    if (At<uint8_t>(scene, 0xdb20) != 0) {
        ThreadPoolGroupWait(At<THREADPOOLGROUP*>(scene, 0xdb10));
    }
}

// ref: FUN_14254fd50
bool SceneOcclusionOn(SceneOcclusion** holder) {
    return (*holder)->Field<uint32_t>(0x80) != 0;
}

// ref: FUN_14254fe60
uint64_t SceneOcclusionResult(SceneOcclusion** holder) {
    return (*holder)->Field<uint64_t>(0x7128);
}

// ---- global switches -----------------------------------------------------------------------

// DAT_142d00e58: the particulate volumes draw (1 in the reference's data).
uint8_t s_particulatesOn = 1;
// DAT_1430a5b92: the world's projected texture target is to be cleared.
uint8_t s_projectedClearPending;
// DAT_1430a5b88: the projected textures' blend (set by the type 6 scene's projector).
float s_projectedBlend;
// DAT_1447cedd8, DAT_1430a5ce8: the outline and spell-effect passes (set by their setup,
// FUN_142557240 and FUN_1412b01d0, not ported).
uint8_t s_outlinesOn;
uint8_t s_spellEffectsOn;
// DAT_142d00c20: the LOD distance the budget is taken from.
float s_lodDistance = 70.0f;
// DAT_1447ce270: the liquids' fog origin and range (FUN_142527290).
float s_liquidFog[4];

// ref: FUN_141396b60
uint8_t ParticulatesOn() {
    return s_particulatesOn;
}

// ref: FUN_1425573c0
uint8_t OutlinesOn() {
    return s_outlinesOn;
}

// ref: FUN_1412b0c10
uint8_t SpellEffectsOn() {
    return s_spellEffectsOn;
}

// ref: FUN_142527290
void LiquidSetFog(const float* fog) {
    std::memcpy(s_liquidFog, fog, sizeof(s_liquidFog));
}

// The projected texture target (scene +0x1950's first word) cleared once when asked.
// ref: FUN_1412a6440
void ProjectedTargetClear(void** projector) {
    if (s_projectedClearPending == 0 || projector == nullptr || *projector == nullptr) {
        return;
    }
    FROST_UNIMPLEMENTED();  // the target cleared through the context (+0xd0 or +0xc8)
    s_projectedClearPending = 0;
}

// ---- the subsystems the world's own scene draws --------------------------------------------

// The particulate volumes (FUN_1413962b0 on the scene's +0x48). Not ported.
// ref: FUN_1413962b0
void ParticulatesDraw(void* manager, GxRenderContext* context, bool rayTracing, const float* view,
    const float* viewInverse, const float* position, const float* heading) {
    (void)manager;
    (void)context;
    (void)rayTracing;
    (void)view;
    (void)viewInverse;
    (void)position;
    (void)heading;
    FROST_UNIMPLEMENTED();
}

// The detail target's draw state (FUN_1413a9a10). Not ported.
// ref: FUN_1413a9a10
void DetailTargetBind(void* target, ShaderEffectContext* effect, const float* view, const float* inverse) {
    (void)target;
    (void)effect;
    (void)view;
    (void)inverse;
    FROST_UNIMPLEMENTED();
}

// The cascaded shadow maps (FUN_1414001f0 sets a cascade set up, FUN_1413d72f0's
// FUN_1412b1920 draws one) and SSAO (FUN_142557630 starts it, FUN_142558160 runs it,
// FUN_142557aa0 ends it); the spell-effect targets (FUN_1412b60f0, FUN_1412b8570,
// FUN_1412b3400, FUN_1412b3e00); the LOD budget (FUN_1413c0230). Not ported.
// ref: FUN_1414001f0
void ShadowCascadesSetup(void* cascades, void* viewState, void* environment, uint32_t which) {
    (void)cascades;
    (void)viewState;
    (void)environment;
    (void)which;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142557630
uint8_t SsaoBegin(GXRENDERTARGETS* targets, FRAMEBUFFERTEMP* temp) {
    (void)targets;
    (void)temp;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1412b60f0
uint8_t SpellTargetsBegin(GXRENDERTARGETS* targets, void* state) {
    (void)targets;
    (void)state;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1412b8570
void ResolveTargetsBegin(WORLDRENDERRESOLVE* resolve, GXRENDERTARGETS* targets, void* state) {
    (void)resolve;
    (void)targets;
    (void)state;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_1413c0230(unsigned long long);
// translated: FUN_1413c0230
void WorldLodBudget(int32_t budget) {
    __try {
        FUN_1413c0230((unsigned long long)budget);
    } __except (GenFault(0x1413c0230ull, GetExceptionInformation())) {
    }
}

// What the depth prepass draws (FUN_1413d6bd0): the world's shadow constants, the map, the
// occluders, and the models' pass 0 when the view draws models.
// ref: FUN_1413d6bd0
void PrepassBody(CWorldSceneRender* render, GxRenderContext* context, ShaderEffectContext* effect) {
    uint32_t flags = ViewOf(render)->m_flags;
    if ((flags >> 0x12 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the world's shadow constants (FUN_1413ff2d0)
    }
    if ((flags >> 0x11 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the map's prepass (FUN_14137c9f0, FUN_1413db040, FUN_14137d840)
    }
    if ((flags >> 0x15 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the occluders (FUN_1413db080, FUN_1413da5f0, FUN_1413dc5b0, FUN_141440e20)
    }
    if ((flags & 0x410000) != 0) {
        M2SceneOf(render)->DrawPass(context, effect, 0);
    }
}

// ref: FUN_1413d69d0
void PrepassDrawThree(CWorldSceneRender* render, GxRenderContext* context) {
    (void)render;
    (void)context;
    FROST_UNIMPLEMENTED();
}

// The depth prepass (FUN_1413d6700): in the view's prepass modes (1, 2), the prepass targets
// cleared (the linear depth to 100000, the normals to their rest value, depth to 0) and bound
// around the prepass; in mode 3, that mode's form; forward (0) has none. `deferred` leaves the
// textures bound.
// ref: FUN_1413d6700
void PrepassDraw(CWorldSceneRender* render, GxRenderContext* context, ShaderEffectContext* effect, bool deferred) {
    int32_t mode = static_cast<int32_t>(ViewOf(render)->m_6b4);
    if (mode == 1 || mode == 2) {
        if (!deferred) {
            context->UnbindTextures();
        }
        GXRENDERTARGETSCLEAR clear = {};
        clear.m_targets = At<GXRENDERTARGETS*>(render, 0x68);
        clear.m_mask = 0x33;
        clear.m_colors[0][0] = 100000.0f;
        clear.m_colors[1][0] = 0.5f;  // DAT_142738e78, DAT_142738e60, DAT_142738e68
        clear.m_colors[1][1] = 0.5f;
        clear.m_colors[1][3] = 1.0f;
        clear.m_depth = 0.0f;
        clear.m_stencil = static_cast<uint8_t>(~(ViewOf(render)->m_flags >> 0xc) & 1);
        context->ClearAndSetTargets(&clear);
        PrepassBody(render, context, effect);
        if (!deferred) {
            context->UnbindTextures();
        }
    } else if (mode == 3) {
        PrepassDrawThree(render, context);
    }
}

// The water view's render state (FUN_1413d3a00, view flag 0x800). Not ported.
// ref: FUN_1413d3a00
void WaterViewDraw(CWorldSceneRender* render, GxRenderContext* context) {
    if ((ViewOf(render)->m_flags & 0x800) == 0) {
        return;
    }
    (void)context;
    FROST_UNIMPLEMENTED();
}

// The sun's shadow map and the map's fading objects' shadows (FUN_1413d72f0). Not ported.
// ref: FUN_1413d72f0
void ShadowsDraw(CWorldSceneRender* render, GxRenderContext* context) {
    (void)render;
    (void)context;
    FROST_UNIMPLEMENTED();
}

// The linear depth copy for the cascades (the "LinearDepth copy" pipelines at
// DAT_1430ad328). Not ported.
void LinearDepthCopy(CWorldSceneRender* render, GxRenderContext* context) {
    (void)render;
    (void)context;
    FROST_UNIMPLEMENTED();
}

// The projected textures over the scene (FUN_1412a7350, with the type 6 scene's projector).
// Not ported.
// ref: FUN_1412a7350
void ProjectedTexturesDraw(CWorldSceneRender* render, GxRenderContext* context) {
    (void)render;
    (void)context;
    FROST_UNIMPLEMENTED();
}

// The outline and spell-effect passes after the scene (FUN_142557490, FUN_142557410,
// FUN_1412b14d0, FUN_1412b1680, FUN_1412b0c80). Not ported.
// ref: FUN_142557490
void OutlinesBegin(void* state, GXRENDERTARGETS* targets) {
    (void)state;
    (void)targets;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412b14d0
void SpellEffectsBegin(FRAMEBUFFER* buffer, GXRENDERTARGETS* targets) {
    (void)buffer;
    (void)targets;
    FROST_UNIMPLEMENTED();
}

// The scene's water view lists waited on (FUN_1413a2e60 on each of the view's lists).
// ref: FUN_1413a2e60
void WorldViewListWait(WORLDVIEWLIST* list) {
    if (list->m_held != 0) {
        ThreadPoolGroupWait(list->m_group);
        list->m_2c = list->m_held;
    }
}

// ref: FUN_1413dd150
void WorldViewListsWait(CWorldView* view) {
    for (WORLDVIEWLIST& list : view->m_lists) {
        WorldViewListWait(&list);
    }
}

// The cull's rectangle checks (FUN_141444ce0, FUN_141444d60) and its job wait (FUN_1414494f0).
// ref: FUN_141444ce0
void CWorldCullResetRect(CWorldCull* cull) {
    (void)cull;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141444d60
void CWorldCullSetRect(CWorldCull* cull, uint32_t a2) {
    (void)cull;
    (void)a2;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1414494f0
void CWorldCullWait(CWorldCull* cull) {
    if ((At<uint8_t>(cull, 0x16284) & 0x40) != 0 && At<int32_t>(cull, 0x16288) != 0) {
        ThreadPoolGroupWait(At<THREADPOOLGROUP*>(cull, 0x16570));
    }
}

// Whether the cull found the camera's water box (FUN_1413dcc50).
// ref: FUN_1413dcc50
bool WorldViewHasWaterBox(CWorldView* view) {
    CWorldCull* cull = view->m_cull;
    return At<float>(cull, 0x1624c) < At<float>(cull, 0x16254) && At<float>(cull, 0x16248) < At<float>(cull, 0x16250);
}

// ---- the frame's environment and clear -----------------------------------------------------

// ref: FUN_1412afa10
uint32_t EnvironmentFogMode(const uint8_t* environment, uint32_t mode) {
    if (mode == 1) {
        return *reinterpret_cast<const uint32_t*>(environment + 0x5b8);
    }
    return 1;
}

// The fog block the frame's environment constants use (FUN_142442700): the second light's
// for an env that asks or a held light, else the near (or far) one.
// ref: FUN_142442700
MODELFOG* DayNightEnvironmentFog(DAYNIGHT* dayNight, const uint8_t* environment, bool farFog) {
    uint8_t* scene = reinterpret_cast<uint8_t*>(dayNight->m_scene);
    if (environment[0x5bc] == 0 && scene[0x1601] == 0) {
        return reinterpret_cast<MODELFOG*>(scene + (farFog ? 0x1b0 : 0xf8));
    }
    return reinterpret_cast<MODELFOG*>(scene + 0x250);
}

// The fog the frame clears to without a water box (FUN_1424427c0).
// ref: FUN_1424427c0
MODELFOG* DayNightClearFog(DAYNIGHT* dayNight, const uint8_t* environment) {
    uint8_t* scene = reinterpret_cast<uint8_t*>(dayNight->m_scene);
    size_t offset = 0x2f4;
    if (environment[0x5bc] != 0 || scene[0x1601] != 0) {
        offset = 0x394;
    }
    return reinterpret_cast<MODELFOG*>(scene + offset);
}

// Whether the light is the map's own (no held light, and computed or no sky model).
// ref: FUN_1424439b0
bool DayNightOwnLight(DAYNIGHT* dayNight) {
    uint8_t* scene = reinterpret_cast<uint8_t*>(dayNight->m_scene);
    return scene[0x1601] == 0 && (*reinterpret_cast<int32_t*>(scene + 0x15f0) != 0 || scene[0x1bf8] == 0);
}

// The fog constants into the effect context (+0x644.., FUN_1412adfd0).
// ref: FUN_1412adfd0
void ShaderEffectSetFogConstants(ShaderEffectContext* effect, const float* constants, float scale) {
    auto bytes = reinterpret_cast<uint8_t*>(effect);
    std::memcpy(bytes + 0x644, constants, 0x40);
    bytes[0x904] = 1;
    *reinterpret_cast<float*>(bytes + 0x7bc) = scale;
}

// The projected texture (or a transparent black volume) at pixel slot 0x10, and its sampler.
// ref: FUN_1412aa810
void ProjectedTextureBind(void** projector, GxRenderContext* context) {
    void* tex = (projector == nullptr || *projector == nullptr) ? TextureTransparentBlack3DGxTex() : *projector;
    context->BindTexture(2, 0x10, static_cast<gxDx12Backend::Tex*>(tex), 0);
    context->SetSampler(2, 0xd, 0x0000800200000001ull);     // DAT_142cfc340
}

}  // namespace

// The fog constants for the frame's effect context: the fog mode's count three times, the fog
// colour set by mode (a single layer for mode 1), and the projected blend.
// ref: FUN_1413d6ec0
void CWorldSceneRender::SetEnvironmentConstants(GxRenderContext* context, ShaderEffectContext* effect, bool farFog) {
    if (DayNightOf(this) == nullptr) {
        return;
    }
    CWorldScene* scene = SceneOf(this);
    uint32_t mode = 8;
    auto environment = reinterpret_cast<const uint8_t*>(&scene->m_farClip);
    if (scene->m_67c[2] != 0) {
        MODELFOG* fog = DayNightEnvironmentFog(DayNightOf(this), environment, farFog);
        mode = fog->m_words[0x16];
    }
    uint32_t layers = EnvironmentFogMode(environment, mode);
    auto constants = reinterpret_cast<float*>(&this->m_f50[0]);
    float count = static_cast<float>(mode);
    constants[0] = count;
    constants[1] = count;
    constants[2] = count;
    constants[3] = 0.0f;
    if (mode == 1) {
        constants[8] = 2.0f;
        constants[9] = 3.0f;
        constants[10] = 4.0f;
        constants[11] = static_cast<float>(layers);
    } else {
        // DAT_142721a50
        constants[8] = 0.0f;
        constants[9] = 1.0f;
        constants[10] = 2.0f;
        constants[11] = 3.0f;
    }
    constants[4] = 0.0f;
    constants[5] = 0.0f;
    constants[6] = 0.0f;
    constants[7] = s_projectedBlend;
    ShaderEffectSetFogConstants(effect, constants, 1.0f);
    ProjectedTextureBind(static_cast<void**>(scene->m_1950), context);
}

// The frame's colour (the fog's, the scene's clear override, or black) and depth cleared: in
// one clear over the whole viewport, else the depth cleared and the colour drawn into the
// viewport.
// ref: FUN_1413d6d00
void CWorldSceneRender::ClearFrame(GXRENDERTARGETS* targets, bool clearDepth) {
    CWorldScene* scene = SceneOf(this);
    CWorldView* view = ViewOf(this);
    uint32_t color = 0;
    auto environment = reinterpret_cast<const uint8_t*>(&scene->m_farClip);
    if (RenderSettingsDepthFillMode(&g_renderSettings)) {
        color = scene->m_1968;
    } else if (this->m_040[2] != 0) {
        DAYNIGHT* dayNight = DayNightOf(this);
        if (!WorldViewHasWaterBox(view)) {
            if (dayNight != nullptr) {
                color = DayNightClearFog(dayNight, environment)->m_words[0];
            }
        } else {
            bool own = dayNight != nullptr && DayNightOwnLight(dayNight);
            bool useOverride = (view->m_flags >> 0xc & 1) != 0;
            if (!useOverride && (view->m_flags >> 0x14 & 1) != 0) {
                bool skip = scene->m_67c[0] != 0 && WorldWaterDetail() == 0;
                useOverride = !skip && own;
            }
            if (useOverride) {
                color = scene->m_1968;
            } else if (dayNight != nullptr) {
                color = DayNightEnvironmentFog(dayNight, environment, false)->m_words[0];
            }
        }
    }
    GxRenderContext* context = g_gxRenderContext;
    uint8_t stencil = static_cast<uint8_t>(~(view->m_flags >> 0xc) & 1);
    const float* viewport = &context->Field<float>(0x1c);
    // Diverges: the frame's rectangle also counts when it is only the scissor. A UI model scene
    // draws through its rectangle's projection and scissor with the viewport left whole, so the
    // whole-target clear painted the fog colour (white for a scene DayNight never computes) over
    // the window outside the glue's 16:9 area, which the reference leaves black. Where the
    // reference narrows the viewport for that frame is not found yet.
    const float* scissor = &context->Field<float>(0x78);
    bool wholeScissor = scissor[0] <= 0.0f && scissor[1] <= 0.0f && 1.0f <= scissor[2] && 1.0f <= scissor[3];
    if (wholeScissor && viewport[0] == 0.0f && viewport[2] == 0.0f && viewport[1] == 1.0f && viewport[3] == 1.0f) {
        context->Clear(targets, clearDepth ? 0x31 : 1, color, 0.0f, stencil, 0);
        return;
    }
    if (clearDepth) {
        context->Clear(targets, 0x30, color, 0.0f, stencil, 0);
    } else {
        context->SetRenderTargets(targets);
    }
    FROST_UNIMPLEMENTED();  // the context's viewport colour clear (+0xb0, mask 0xf)
}

// ref: FUN_1413cecc0
void* CWorldSceneRender::GetCommandList(uint32_t slot) {
    if (!this->UsesCommandLists(slot)) {
        return nullptr;
    }
    FROST_UNIMPLEMENTED();  // the slot's deferred command list, made on first use (FUN_142289ad0)
    return nullptr;
}

// Whether a pass records into a deferred command list: only with ray tracing on (+0x9d2), by
// the render settings' per-pass switches.
// ref: FUN_1413cee80
bool CWorldSceneRender::UsesCommandLists(uint32_t slot) {
    if (this->m_9c8[0xa] == 0) {
        return false;
    }
    uint32_t flags = g_renderSettings.m_flags;
    uint8_t lists[0x19] = {};
    lists[0] = (flags >> 0x12) & 1;
    lists[1] = (flags >> 0x13) & 1;
    lists[2] = (flags >> 0x14) & 1;
    lists[3] = lists[2];
    lists[4] = lists[2];
    lists[5] = lists[2];
    lists[6] = (flags >> 0x15) & 1;
    lists[7] = (flags >> 0x16) & 1;
    lists[8] = (flags >> 0x17) & 1;
    uint8_t high = static_cast<uint8_t>(flags >> 0x18);
    lists[0x10] = high & 1;
    lists[0x11] = high >> 1 & 1;
    lists[0x15] = high >> 2 & 1;
    lists[0x16] = high >> 3 & 1;
    lists[0x17] = (flags >> 0x17) & 1;
    lists[0x18] = high >> 4 & 1;
    return slot < sizeof(lists) && lists[slot] != 0;
}

// The opaque pass: the weather constants, the M2 scene's targets and opaque passes (6, then
// 7) between the shading rate's set and reset, the world's terrain, map objects and sky where
// the view asks, and the render callbacks' stage 3.
// ref: FUN_1413d47c0
void CWorldSceneRender::DrawOpaque(FRAMEBUFFER* buffer) {
    (void)buffer;
    GxRenderContext* context = g_gxRenderContext;
    ShaderEffectContext* effect = this->m_effectContext;
    CWorldScene* scene = SceneOf(this);
    CWorldView* view = ViewOf(this);
    CM2Scene* m2Scene = M2SceneOf(this);
    float direction[3];
    TransformPoint(view->m_matrices[2], MapWeatherDirection(scene->m_038), direction);
    float density = MapWeatherDensity(scene->m_038);
    const float* color = MapWeatherColor(scene->m_038);
    float* constants = context->LockConstants(2, 7, 0x20);
    constants[0] = direction[0];
    constants[1] = direction[1];
    constants[2] = direction[2];
    constants[3] = density;
    constants[4] = color[0];
    constants[5] = color[1];
    constants[6] = color[2];
    constants[7] = 1.0f;
    context->UnlockConstants(2, 7);
    uint32_t flags = view->m_flags;
    if (m2Scene != nullptr) {
        At<uint64_t>(m2Scene, 0xdb00) = At<uint64_t>(this, 0x1008);
        At<uint64_t>(m2Scene, 0xdaf8) = At<uint64_t>(this, 0x1010);
        if (g_renderSettings.m_msaaSamples != 0) {
            m2Scene->DrawPass(context, effect, 5);
        }
    }
    if ((flags >> 0x12 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the world's shadowed map objects (FUN_1414001f0, FUN_1413d42f0)
    }
    bool opaqueM2 = (flags & 0x410000) != 0;
    if (opaqueM2) {
        if (this->GetCommandList(1) == nullptr) {
            scene->BeginShadingRate(context);
            m2Scene->DrawPass(context, effect, 6);
            scene->EndShadingRate(context);
        }
    }
    if ((flags & 0x220000) != 0 && this->GetCommandList(7) == nullptr) {
        FROST_UNIMPLEMENTED();  // the terrain (FUN_1413d74a0)
    }
    if ((flags >> 0x14 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the map objects (FUN_1413d7030)
    }
    if ((flags >> 0xe & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the sky (FUN_142442cb0, FUN_1412aeab0, FUN_14252ccd0)
    }
    if (WorldWaterDetail() > 1 && (At<uint8_t>(view->m_cull, 0x161e8) >> 2 & 1) != 0 && (flags & 0x10) != 0) {
        FROST_UNIMPLEMENTED();  // the refraction copy (FUN_1412a0f80 with FUN_1413cb870)
    }
    if (opaqueM2 && this->GetCommandList(2) == nullptr) {
        scene->BeginShadingRate(context);
        m2Scene->DrawPass(context, effect, 7);
        scene->EndShadingRate(context);
    }
    if ((flags >> 0x1d & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the view's map object batches (FUN_1413cc410, FUN_1413cc7d0)
    }
    if ((flags >> 0xf & 1) != 0) {
        context->UnbindTextures();
        FRAMEBUFFER* sceneBuffer = SceneBufferOf(this);
        struct {
            uint64_t m_shared;
            GXRENDERTARGETS* m_targets;
            CWorldScene* m_scene;
            CWorldView* m_view;
        } frame = {At<uint64_t>(this, 0x68), TargetsOrNull(sceneBuffer), scene, view};
        scene->RunCallbacks(3, &frame);
    }
}

namespace {

// The cull's underwater bit (+0x161e8 bit 7).
// ref: FUN_1413dcb40
bool WorldViewUnderwater(CWorldView* view) {
    return (At<uint8_t>(view->m_cull, 0x161e8) >> 7) != 0;
}

// The world's graphics feature bits (DAT_1430ab9d8 & DAT_1430ab9d4), set by the world's
// options; zero until those are ported.
uint32_t s_worldFeatures;
uint32_t s_worldFeaturesMask;

// ref: FUN_14134e3d0
uint32_t WorldFeatures() {
    return s_worldFeatures & s_worldFeaturesMask;
}

// DAT_1430a1fa0: the targets the alpha M2 passes draw into.
GXRENDERTARGETS* s_alphaTargets;

}  // namespace

// The pass drawn on top of the scene with its own near projection (pass 11): the viewport's
// depth range narrowed to [0, 0.99912] and the M2 scene given a vertical-fov projection from
// the camera, then the viewport put back.
// ref: FUN_1413d4070
void CWorldSceneRender::DrawFrontPass(GxRenderContext* context, ShaderEffectContext* effect) {
    float* viewport = &context->Field<float>(0x1c);
    float x0 = viewport[0];
    float x1 = viewport[1];
    float y0 = viewport[2];
    float y1 = viewport[3];
    float z0 = viewport[4];
    float z1 = viewport[5];
    float cx0 = x0 > 0.0f ? x0 : 0.0f;
    float cx1 = x1 < 1.0f ? x1 : 1.0f;
    float cy0 = y0 > 0.0f ? y0 : 0.0f;
    float cy1 = y1 < 1.0f ? y1 : 1.0f;
    const float nearRange = 0.99912107f;    // DAT_142738e50
    CM2Scene* m2Scene = SceneOf(this)->m_m2Scene;
    if (x0 != cx0 || x1 != cx1 || y0 != cy0 || y1 != cy1 || z0 != 0.0f || z1 != nearRange) {
        viewport[0] = cx0;
        context->Field<uint8_t>(0x18) = 1;
        viewport[1] = cx1;
        viewport[2] = cy0;
        viewport[3] = cy1;
        viewport[4] = 0.0f;
        viewport[5] = nearRange;
    }
    CWorldView* view = ViewOf(this);
    float projection[16];
    MatrixIdentity(projection);
    MatrixPerspectiveVertical(view->m_camera.m_fov, view->m_camera.m_aspect, view->m_cameraLast.m_nearClip,
        view->m_cameraLast.m_farClip, projection);
    float minimum[3] = {x0, y0, 0.0f};
    float maximum[3] = {x1, y1, nearRange};
    m2Scene->SetView(reinterpret_cast<const C3Vector*>(view->m_camera.m_position),
        reinterpret_cast<const C44Matrix*>(view->m_matrices[2]), reinterpret_cast<const C44Matrix*>(projection), minimum,
        maximum, false);
    m2Scene->DrawPass(context, effect, 0xb);
    if (cx0 != viewport[0] || cx1 != viewport[1] || cy0 != viewport[2] || cy1 != viewport[3] || z0 != viewport[4]
        || z1 != viewport[5]) {
        viewport[0] = cx0;
        viewport[1] = cx1;
        viewport[2] = cy0;
        viewport[3] = cy1;
        viewport[4] = z0;
        viewport[5] = z1;
        context->Field<uint8_t>(0x18) = 1;
    }
}

// The projected texture's two passes over the alpha M2s (the occlusion's result with the
// cull's occluders; their draws are empty functions in the reference build). Not ported past
// its gate.
// ref: FUN_1413d8700
void CWorldSceneRender::DrawProjectedPass(GxRenderContext* context, ShaderEffectContext* effect, bool second) {
    (void)effect;
    (void)second;
    CWorldView* view = ViewOf(this);
    uint64_t occlusion = 0;
    if (SceneOcclusionOn(&view->m_occlusion)) {
        occlusion = SceneOcclusionResult(&view->m_occlusion);
    }
    auto occluders = static_cast<uint32_t*>(CWorldCullOccluders(view->m_cull));
    if (*occluders == 0 || At<uint64_t>(this, 0x68) == 0 || occlusion == 0 || At<uint64_t>(this, 0xf90) == 0) {
        return;
    }
    ProjectedTextureBind(static_cast<void**>(SceneOf(this)->m_1950), context);
    FROST_UNIMPLEMENTED();  // the copy of the projected target (+0x198 or +0x1a8)
}

// The alpha M2 passes: 9, the projected texture's two, 8 and 12.
// ref: FUN_1413d2380
void CWorldSceneRender::DrawAlphaM2(GxRenderContext* context, ShaderEffectContext* effect) {
    CM2Scene* m2Scene = M2SceneOf(this);
    m2Scene->DrawPass(context, effect, 9);
    this->DrawProjectedPass(context, effect, false);
    this->DrawProjectedPass(context, effect, true);
    m2Scene->DrawPass(context, effect, 8);
    m2Scene->DrawPass(context, effect, 0xc);
}

// The particulates' alpha draws (FUN_1413cf2c0's gate; FUN_1413970d0, FUN_141396b70). Only
// a view that draws them (flags & 0xa == 8) with DayNight and particulates asks.
// ref: FUN_1413cf2c0
bool CWorldSceneRender::PrepareParticulates() {
    CWorldScene* scene = SceneOf(this);
    if (!ParticulatesOn() || DayNightOf(this) == nullptr || (ViewOf(this)->m_flags & 0xa) != 8
        || At<uint64_t>(scene->m_particulates, 8) == 0) {
        return false;
    }
    FROST_UNIMPLEMENTED();  // the particulate volumes' draw lists
    return true;
}

// The transparent and composite pass: the cascades for alpha, the copy target, and above or
// under water the alpha M2s, particulates, liquids and sky; then the render callbacks' stage
// 4. The world's own scene parts are stubs.
// ref: FUN_1413d07b0
void CWorldSceneRender::DrawTransparent(GXRENDERTARGETS* targets, GXRENDERTARGETS* targets2) {
    (void)targets;
    (void)targets2;
    CWorldScene* scene = SceneOf(this);
    CWorldView* view = ViewOf(this);
    ShadowCascadesSetup(&this->m_projections[2], &At<uint8_t>(view, 0x38), &scene->m_farClip, 2);
    GxRenderContext* context = g_gxRenderContext;
    ShaderEffectContext* effect = this->m_effectContext;
    uint32_t flags = view->m_flags;
    if ((flags >> 0x12 & 1) != 0) {
        FROST_UNIMPLEMENTED();  // the world's shadow constants (FUN_1412ad7f0, FUN_1412adbf0, FUN_1412b5cf0, FUN_1413ff2d0)
    }
    if ((flags >> 0x17 & 1) != 0 && (WorldFeatures() & 0x40) != 0) {
        FROST_UNIMPLEMENTED();  // FUN_1413d3610
    }
    if ((flags >> 0x13 & 1) != 0 && static_cast<int8_t>(WorldFeatures()) < 0) {
        FROST_UNIMPLEMENTED();  // the liquids (FUN_1413f5d70, FUN_1413f6660)
    }
    bool copyTarget = false;
    auto occluders = static_cast<uint32_t*>(CWorldCullOccluders(view->m_cull));
    if (*occluders != 0) {
        FROST_UNIMPLEMENTED();  // the occluders' sort (FUN_1413cb8b0)
        copyTarget = At<uint64_t>(this, 0x68) != 0;
    }
    At<uint64_t>(this, 0xf90) = 0;
    if (copyTarget) {
        FROST_UNIMPLEMENTED();  // the "ExecuteCopyDiscard" copy of the shared colour (FUN_14129f180)
    }
    FRAMEBUFFER* sceneBuffer = SceneBufferOf(this);
    if (view->m_690 == 0 && !WorldViewUnderwater(view)) {
        bool opaqueM2 = (flags & 0x410000) != 0;
        if ((flags & 0x4010) == 0x4010 && WorldWaterDetail() > 1) {
            FROST_UNIMPLEMENTED();  // the water surface's sky (FUN_142528480, FUN_1413d28f0)
        }
        if (opaqueM2) {
            if (this->GetCommandList(0x17) == nullptr) {
                M2SceneOf(this)->DrawPass(context, effect, 10);    // FUN_1413d2560
            }
        }
        if ((flags & 0x4010) == 0x4010) {
            FROST_UNIMPLEMENTED();  // FUN_142528480
        }
        if ((flags & 0x10) != 0 && WorldWaterDetail() >= 3 && view->WantsReflection()) {
            FROST_UNIMPLEMENTED();  // the reflection's composite (FUN_1425285f0)
        }
        if ((flags & 0x40) != 0) {
            FROST_UNIMPLEMENTED();  // FUN_1405e8500
        }
        bool particulates = this->PrepareParticulates();
        if (particulates) {
            FROST_UNIMPLEMENTED();  // the particulates (FUN_1413d59b0, 0x11)
        }
        if ((flags >> 0xe & 1) != 0) {
            FROST_UNIMPLEMENTED();  // the sky's alpha (FUN_1413d2830)
        }
        if ((flags >> 0x19 & 1) != 0) {
            FROST_UNIMPLEMENTED();  // FUN_1413d2f00
        }
        if ((flags >> 0x1d & 1) != 0) {
            FROST_UNIMPLEMENTED();  // FUN_1413d2fd0
        }
        if (particulates) {
            FROST_UNIMPLEMENTED();  // the particulates (FUN_1413d59b0, 0x12)
        }
        if (opaqueM2) {
            this->DrawFrontPass(context, effect);
            s_alphaTargets = TargetsOrNull(sceneBuffer);
            if (this->GetCommandList(8) == nullptr) {
                this->DrawAlphaM2(context, effect);
            }
            if (copyTarget) {
                FROST_UNIMPLEMENTED();  // the copy target handed back
            }
        }
        ThreadPoolGroupWait(this->m_group);
        if ((view->m_flags & 2) == 0 && scene->m_type == 6) {
            FROST_UNIMPLEMENTED();  // the anima copy (FUN_1413d2a60)
        }
        if (particulates) {
            FROST_UNIMPLEMENTED();  // the particulates (FUN_1413d59b0, 0x13)
        }
        if ((flags >> 0x18 & 1) != 0) {
            context->SetRenderTargets(TargetsOrNull(sceneBuffer));
            FROST_UNIMPLEMENTED();  // FUN_141390550
        }
        if ((flags & 0x40) != 0) {
            FROST_UNIMPLEMENTED();  // FUN_1413d2f70
            context->SetRenderTargets(TargetsOrNull(sceneBuffer));
        }
    } else {
        FROST_UNIMPLEMENTED();  // the underwater or occluded order of the same passes
    }
    if ((flags >> 0xf & 1) != 0) {
        struct {
            uint64_t m_shared;
            GXRENDERTARGETS* m_targets;
            CWorldScene* m_scene;
            CWorldView* m_view;
        } frame = {At<uint64_t>(this, 0x68), TargetsOrNull(sceneBuffer), scene, view};
        context->SetRenderTargets(TargetsOrNull(sceneBuffer));
        scene->RunCallbacks(4, &frame);
    }
    if ((flags >> 0x1a & 1) != 0) {
        if ((WorldFeatures() >> 10 & 1) != 0 && view->m_690 != 0 && this->m_9a8 != 0) {
            FROST_UNIMPLEMENTED();  // FUN_1414256f0
        }
        context->SetRenderTargets(TargetsOrNull(sceneBuffer));
    }
    if ((flags >> 0x1b & 1) != 0) {
        FROST_UNIMPLEMENTED();  // FUN_1413d3840
    }
}

// The view's passes: the M2 scene given its clip plane, occlusion, elements and weather; the
// particulates; the environment constants; the frame cleared; the cascades and SSAO; the
// outline pass; the opaque and transparent passes; and the after-passes.
// ref: FUN_1413cf390
void CWorldSceneRender::Draw() {
    GxRenderContext* context = g_gxRenderContext;
    CWorldScene* scene = SceneOf(this);
    if (scene->m_1950 != nullptr) {
        At<uint64_t>(this->m_effectContext, 0x908) = 0;
    }
    CWorldView* view = ViewOf(this);
    if (view->m_flags & 0x400) {
        DetailTargetBind(view->m_238, this->m_effectContext, view->m_matrices[2], view->m_matrices[3]);
    }
    CM2Scene* m2Scene = M2SceneOf(this);
    if (m2Scene != nullptr) {
        uint64_t occlusion = 0;
        if (*static_cast<uint32_t*>(CWorldCullOccluders(view->m_cull)) > 0) {
            if (SceneOcclusionOn(&view->m_occlusion)) {
                occlusion = SceneOcclusionResult(&view->m_occlusion);
            }
        }
        At<uint64_t>(m2Scene, 0xdb28) = occlusion;
        if ((view->m_flags >> 7 & 1) != 0) {
            float plane[4];
            std::memcpy(plane, view->m_clipPlane, 0x10);
            PlaneTransform(plane, view->m_matrices[1]);
            M2SceneSetClipPlane(m2Scene, true, plane);
        } else {
            const float none[4] = {0.0f, 0.0f, 0.0f, 1.0f};     // DAT_14261aa10
            M2SceneSetClipPlane(m2Scene, false, none);
        }
        At<uint64_t>(m2Scene, 0xdaf0) = At<uint64_t>(this, 0x68);
        At<uint32_t>(m2Scene, 0xdb08) = At<uint32_t>(this, 0x9c8);
        m2Scene->BuildElements(this->m_9c8[8], this->m_9c8[9]);
        float direction[3];
        TransformPoint(view->m_matrices[2], MapWeatherDirection(scene->m_038), direction);
        float density = MapWeatherDensity(scene->m_038);
        const float* color = MapWeatherColor(scene->m_038);
        float extra = MapWeatherIntensity(scene->m_038);
        float* weather = &At<float>(m2Scene, 0xd644);
        weather[0] = color[0];
        weather[1] = color[1];
        weather[2] = color[2];
        weather[3] = extra;
        weather[4] = direction[0];
        weather[5] = direction[1];
        weather[6] = direction[2];
        weather[7] = density;
    }
    uint32_t flags = view->m_flags;
    if ((flags & 2) == 0 && ParticulatesOn() && GxDeviceCaps()[0xad] != 0) {
        bool rayTracing = this->m_9c8[0xa] != 0 && (g_renderSettings.m_flags >> 0x18 & 1) != 0 && scene->m_views.size() == 1
            && GxCurrentApi() == 2;
        if (rayTracing) {
            FROST_UNIMPLEMENTED();  // the ray-traced particulates' context (FUN_1413c4320, FUN_1413c8020)
        }
        ParticulatesDraw(scene->m_particulates, context, rayTracing, view->m_matrices[2], view->m_matrices[3],
            view->m_camera.m_position, &reinterpret_cast<float*>(view)[0x460 / 4]);
    }
    if (this->GetCommandList(0x10) != nullptr) {
        FROST_UNIMPLEMENTED();  // the particulate occlusion job (FUN_1413d5940)
    }
    ProjectedTargetClear(static_cast<void**>(scene->m_1950));
    if ((view->m_flags & 0x1000) == 0) {
        const float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        context->SetScissor(full);
    }
    this->SetEnvironmentConstants(context, this->m_effectContext, false);
    const float liquidFog[4] = {view->m_camera.m_position[0], view->m_camera.m_position[1], 17066.666f, 2.925e-05f};
    LiquidSetFog(liquidFog);
    uint8_t outlines = 0;
    uint8_t spellEffects = 0;
    uint8_t pass4 = 0;
    int32_t cascades = static_cast<int32_t>(view->m_6b4);
    GxRenderTargetsClear(&At<GXRENDERTARGETS>(this, 0x560));
    GxRenderTargetsClear(&At<GXRENDERTARGETS>(this, 0x650));
    this->m_resolve.OnDeviceDestroyed();
    const float* rect = &At<float>(view->m_cull, 0x1620c);
    if (rect[3] <= rect[1] || rect[2] <= rect[0]) {
        CWorldCullResetRect(view->m_cull);
        CWorldCullSetRect(view->m_cull, 1);
    }
    CWorldCullWait(view->m_cull);
    if (cascades == 0) {
        this->m_9d4 = 0;
    } else {
        ShadowCascadesSetup(&this->m_projections[0], &At<uint8_t>(view, 0x38), &scene->m_farClip, 0);
        auto shared = At<GXRENDERTARGETS*>(this, 0x68);
        this->m_9d4 = SsaoBegin(shared, &this->m_temp9d8);
        this->m_b18 = SpellTargetsBegin(shared, &this->m_tempsB20[0]);
        ResolveTargetsBegin(&this->m_resolve, shared, &this->m_tempsB20[0]);
        WorldLodBudget((1 - static_cast<int32_t>(s_lodDistance * -0.24f)) * 8);    // DAT_142738e54
        if (this->GetCommandList(0) == nullptr) {
            PrepassDraw(this, context, this->m_effectContext, false);
            if (this->m_9d4 != 0) {
                FROST_UNIMPLEMENTED();  // SSAO (FUN_142558160)
            }
            if (this->m_b18 != 0) {
                FROST_UNIMPLEMENTED();  // the spell effect targets (FUN_1412b3400)
            }
            WaterViewDraw(this, context);
        }
        if (view->m_flags & 0x400) {
            FROST_UNIMPLEMENTED();  // the detail target pass (FUN_1413c4d20, FUN_1413c4a10, FUN_1412b4f30)
        }
        if (this->m_9d4 != 0) {
            FROST_UNIMPLEMENTED();  // SSAO's end (FUN_142557aa0)
        }
        if (this->m_b18 != 0) {
            FROST_UNIMPLEMENTED();  // the spell effect targets' end (FUN_1412b3e00)
        }
        ShadowsDraw(this, context);
    }
    if (m2Scene != nullptr) {
        M2SceneWaitJobs(m2Scene);
        if (cascades != 0 && (flags >> 0x1c & 1) != 0 && At<uint64_t>(m2Scene, 0xfb0) != 0 && OutlinesOn()) {
            outlines = 1;
        }
        if ((flags >> 0xd & 1) != 0 && (At<uint64_t>(m2Scene, 0xfc8) != 0 || At<uint64_t>(m2Scene, 0xfe0) != 0)
            && SpellEffectsOn()) {
            spellEffects = 1;
        }
        pass4 = At<uint64_t>(m2Scene, 0xff8) != 0 ? 1 : 0;
    }
    if (outlines) {
        OutlinesBegin(nullptr, At<GXRENDERTARGETS*>(this, 0x68));
        m2Scene->DrawPass(context, this->m_effectContext, 1);
        context->UnbindTextures();
    }
    if (s_projectedBlend != 0.0f) {
        if ((g_renderSettings.m_flags & 0x20) != 0 && m2Scene != nullptr && scene->m_1950 != nullptr) {
            ProjectedTexturesDraw(this, context);
        } else {
            At<uint64_t>(this->m_effectContext, 0x908) = 0;
        }
    }
    WorldViewListsWait(view);
    FRAMEBUFFER* sceneBuffer = SceneBufferOf(this);
    if (cascades != 0 && sceneBuffer->m_targets.m_texs[2] != nullptr) {
        LinearDepthCopy(this, context);
    }
    this->ClearFrame(TargetsOrNull(sceneBuffer), (view->m_6b4 & 0xfffffffd) == 0);
    if (scene->m_19c8 != nullptr) {
        FROST_UNIMPLEMENTED();  // the scene's second target (+0x19c0) through the context's +0xe0
    }
    GXRENDERTARGETS* targets560 = &At<GXRENDERTARGETS>(this, 0x560);
    At<GXRENDERTARGETS*>(this, 0x1008) = targets560->m_count != 0 ? targets560 : nullptr;
    At<uint64_t>(this, 0x1010) = this->m_resolve.m_00;
    GxRenderTargetsClear(&At<GXRENDERTARGETS>(this, 0x470));
    GxRenderTargetsClear(&At<GXRENDERTARGETS>(this, 0x830));
    this->DrawOpaque(&this->m_buffers3f0[0]);
    FrameBufferRelease(&this->m_buffers3f0[1]);
    this->m_resolve.Release();
    if (s_projectedBlend == 0.0f) {
        if ((g_renderSettings.m_flags & 0x20) != 0 && m2Scene != nullptr && scene->m_1950 != nullptr) {
            ProjectedTexturesDraw(this, context);
        } else {
            At<uint64_t>(this->m_effectContext, 0x908) = 0;
        }
    }
    GXRENDERTARGETS* targets470 = &At<GXRENDERTARGETS>(this, 0x470);
    GXRENDERTARGETS* targets650 = &At<GXRENDERTARGETS>(this, 0x650);
    this->DrawTransparent(targets470->m_count != 0 ? targets470 : nullptr, targets650->m_count != 0 ? targets650 : nullptr);
    FrameBufferRelease(&this->m_buffers3f0[0]);
    FrameBufferRelease(&this->m_buffers3f0[4]);
    FrameBufferRelease(&this->m_buffers3f0[2]);
    if (pass4) {
        float& phase = At<float>(m2Scene, 0xb4);
        phase = FrameTime_GetElapsedSeconds() * 0.06f + phase;  // FUN_1412b6d70 (DAT_142727750)
        phase = phase - static_cast<float>(static_cast<int32_t>(phase));
        m2Scene->DrawPass(context, this->m_effectContext, 4);
    }
    if (outlines) {
        FROST_UNIMPLEMENTED();  // the outline composite (FUN_142557410)
    }
    if (spellEffects) {
        SpellEffectsBegin(&this->m_buffers3f0[0], At<GXRENDERTARGETS*>(this, 0x68));
        m2Scene->DrawPass(context, this->m_effectContext, 2);
        m2Scene->DrawPass(context, this->m_effectContext, 3);
        FROST_UNIMPLEMENTED();  // the spell effects' composite (FUN_1412b1680, FUN_1412b0c80)
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1405e8500=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14129f180=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412a0f80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412ad7f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412adbf0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412aeab0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b0c80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b1680=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b3400=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b3e00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b4f30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b5cf0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412b6d70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14137c9f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14137d840=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141390550=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413c0230=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413c4320=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413c4a10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413c4d20=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413c8020=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413cb870=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413cb8b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413cc410=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413cc7d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2560=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2830=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d28f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2a60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2f00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2f70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d2fd0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d3610=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d3840=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d42f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d5940=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d59b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d7030=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413d74a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413da5f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413db040=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413db080=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413dc5b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413f5d70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413f6660=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413ff2d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414001f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414256f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141440e20=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142289ad0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142442cb0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142528480=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1425285f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14252ccd0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142557410=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142557aa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142558160=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1412aa810 = (void*)&ProjectedTextureBind;
extern "C" void* const gen_f_1413d3a00 = (void*)&WaterViewDraw;
extern "C" void* const gen_f_1413d6700 = (void*)&PrepassDraw;
extern "C" void* const gen_f_1413d6bd0 = (void*)&PrepassBody;
extern "C" void* const gen_f_1413dcc50 = (void*)&WorldViewHasWaterBox;
extern "C" void* const gen_f_142442700 = (void*)&DayNightEnvironmentFog;
extern "C" void* const gen_f_1424427c0 = (void*)&DayNightClearFog;
extern "C" void* const gen_f_14254fd50 = (void*)&SceneOcclusionOn;
// ---- end of generated globals ----
